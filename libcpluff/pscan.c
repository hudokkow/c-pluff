/*-------------------------------------------------------------------------
 * C-Pluff, a plug-in framework for C
 * Copyright 2007 Johannes Lehtinen
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *-----------------------------------------------------------------------*/

/** @file
 * Plug-in scanning functionality
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include "cpluff.h"
#include "defines.h"
#include "util.h"
#include "internal.h"


/* ------------------------------------------------------------------------
 * Data structures
 * ----------------------------------------------------------------------*/

typedef struct available_plugin_t available_plugin_t;

struct available_plugin_t {

	cp_plugin_info_t *info;
	
	cp_plugin_loader_t *loader;

};


/* ------------------------------------------------------------------------
 * Function definitions
 * ----------------------------------------------------------------------*/

CP_C_API cp_status_t cp_scan_plugins(cp_context_t *context, int flags) {
	hash_t *avail_plugins = NULL;
	list_t *started_plugins = NULL;
	cp_plugin_info_t **plugins = NULL;
	char *pdir_path = NULL;
	int plugins_stopped = 0;
	cp_status_t status = CP_OK;
	
	CHECK_NOT_NULL(context);
	
	cpi_lock_context(context);
	cpi_check_invocation(context, CPI_CF_ANY, __func__);
	cpi_debug(context, N_("Plug-in scan is starting."));
	do {
		lnode_t *lnode;
		hscan_t hscan;
		hnode_t *hnode;
	
		// Copy the list of started plug-ins, if necessary 
		if ((flags & CP_SP_RESTART_ACTIVE)
			&& (flags & (CP_SP_UPGRADE | CP_SP_STOP_ALL_ON_INSTALL))) {
			int i;
			cp_status_t s;

			if ((plugins = cp_get_plugins_info(context, &s, NULL)) == NULL) {
				status = s;
				break;
			}
			if ((started_plugins = list_create(LISTCOUNT_T_MAX)) == NULL) {
				status = CP_ERR_RESOURCE;
				break;
			}
			for (i = 0; plugins[i] != NULL; i++) {
				cp_plugin_state_t state;
				
				state = cp_get_plugin_state(context, plugins[i]->identifier);
				if (state == CP_PLUGIN_STARTING || state == CP_PLUGIN_ACTIVE) {
					char *pid;
				
					if ((pid = strdup(plugins[i]->identifier)) == NULL) {
						status = CP_ERR_RESOURCE;
						break;
					}
					if ((lnode = lnode_create(pid)) == NULL) {
						free(pid);
						status = CP_ERR_RESOURCE;
						break;
					}
					list_append(started_plugins, lnode);
				}
			}
			cpi_release_info(context, plugins);
			plugins = NULL;
		}
		
		// Create a hash for available plug-ins 
		if ((avail_plugins = hash_create(HASHCOUNT_T_MAX, (int (*)(const void *, const void *)) strcmp, NULL)) == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
	
		// Scan plug-in loaders for available plug-ins 
		hash_scan_begin(&hscan, context->env->loaders_to_plugins);
		while ((hnode = hash_scan_next(&hscan)) != NULL) {
			cp_plugin_loader_t *loader = (cp_plugin_loader_t *) hnode_getkey(hnode);
			cp_plugin_info_t **loaded_plugins;
			int i;
			
			// Scan plug-ins using the loader
			cpi_debugf(context, N_("Scanning plug-ins using loader %p."), (void *) loader);
			loaded_plugins = loader->scan_plugins(loader->data, context);
			if (loaded_plugins == NULL) {
				cpi_errorf(context, N_("Plug-in loader %p failed to scan for plug-ins."), (void *) loader);
				continue;
			}

			// Go through the loaded plug-ins
			for (i = 0; loaded_plugins[i] != NULL; i++) {
				cp_plugin_info_t *plugin = loaded_plugins[i];
			
				// Check if equal or later version of the plug-in is already known 
				if ((hnode = hash_lookup(avail_plugins, plugin->identifier)) != NULL) {
					available_plugin_t *ap = hnode_get(hnode);
					cp_plugin_info_t *plugin2 = ap->info;
					if (cpi_vercmp(plugin->version, plugin2->version) > 0) {
						
						// Release plug-in with smaller version number
						hash_delete_free(avail_plugins, hnode);
						free(ap);
						cp_release_info(context, plugin2);
						hnode = NULL;
					}
				}
				
				// If no equal or later version found, use the plug-in
				if (hnode == NULL) {
					available_plugin_t *ap = NULL;
					int hok = 0;
					
					if ((ap = malloc(sizeof(available_plugin_t))) != NULL) {
						memset(ap, 0, sizeof(available_plugin_t));
						ap->info = plugin;
						ap->loader = loader;
						hok = hash_alloc_insert(avail_plugins, plugin->identifier, ap);
						cpi_use_info(context, plugin);
					}
					
					// Report error and release resources on error
					if (!hok) {
						cpi_errorf(context, N_("Plug-in %s version %s could not be loaded due to insufficient system resources."), plugin->identifier, plugin->version);
						if (ap != NULL) {
							free(ap);
						}
						status = CP_ERR_RESOURCE;
					}
				
				}
					
			}
			
			// Release loaded plug-in information
			if (loader->release_plugins != NULL) {
				loader->release_plugins(loader->data, context, loaded_plugins);
			} else {
				int i;
				for (i = 0; loaded_plugins[i] != NULL; i++) {
					cp_release_info(context, loaded_plugins[i]);
				}
				free(loaded_plugins);				
			}
		}
		
		// Install/upgrade plug-ins 
		hash_scan_begin(&hscan, avail_plugins);
		while ((hnode = hash_scan_next(&hscan)) != NULL) {
			available_plugin_t *ap;
			cp_plugin_info_t *plugin;
			cp_plugin_loader_t *loader;
			cp_plugin_t *ip = NULL;
			hnode_t *hn2;
			int s;
			
			ap = hnode_get(hnode);
			plugin = ap->info;
			loader = ap->loader;
			hn2 = hash_lookup(context->env->plugins, plugin->identifier);
			if (hn2 != NULL) {
				ip = hnode_get(hn2);
			}
			
			// Unload the installed plug-in if it is to be upgraded 
			if (ip != NULL
				&& (flags & CP_SP_UPGRADE)
				&& ((ip->plugin->version == NULL && plugin->version != NULL)
					|| (ip->plugin->version != NULL
						&& plugin->version != NULL
						&& cpi_vercmp(plugin->version, ip->plugin->version) > 0))) {
				if ((flags & (CP_SP_STOP_ALL_ON_UPGRADE | CP_SP_STOP_ALL_ON_INSTALL))
					&& !plugins_stopped) {
					plugins_stopped = 1;
					cp_stop_plugins(context);
				}
				s = cp_uninstall_plugin(context, plugin->identifier);
				assert(s == CP_OK);
				ip = NULL;
			}
			
			// Install the plug-in, if to be installed 
			if (ip == NULL) {
				int hok = 0;
				hash_t *loader_plugins;
			
				// First stop all plug-ins if so specified
				if ((flags & CP_SP_STOP_ALL_ON_INSTALL) && !plugins_stopped) {
					plugins_stopped = 1;
					cp_stop_plugins(context);
				}
				
				// Add plug-in to loader map
				loader_plugins = hnode_get(hash_lookup(context->env->loaders_to_plugins, loader));
				assert(loader_plugins != NULL);
				if ((hok = hash_alloc_insert(loader_plugins, plugin->identifier, NULL))) {
					
					// Install new plug-in
					s = cpi_install_plugin(context, plugin, loader);
				}
				
				// Release resources and set status code on failure
				if (!hok || s != CP_OK) {
					if (hok) {
						hash_delete_free(
							loader_plugins,
							hash_lookup(loader_plugins, plugin->identifier)
						);
					}
					if (hok) {
						status = s;
					} else {
						status = CP_ERR_RESOURCE;
					}
					break;
				}
				
			}
			
			// Remove the plug-in from the hash
			free(ap);
			hash_scan_delfree(avail_plugins, hnode);
			cp_release_info(context, plugin);
		}
		
		// Restart stopped plug-ins if necessary 
		if (started_plugins != NULL) {
			lnode = list_first(started_plugins);
			while (lnode != NULL) {
				char *pid;
				int s;
				
				pid = lnode_get(lnode);
				s = cp_start_plugin(context, pid);
				if (s != CP_OK) {
					status = s;
				}
				lnode = list_next(started_plugins, lnode);
			}
		}
		
	} while (0);

	// Report error
	switch (status) {
		case CP_OK:
			cpi_debug(context, N_("Plug-in scan has completed successfully."));
			break;
		case CP_ERR_RESOURCE:
			cpi_error(context, N_("Could not scan all plug-ins due to insufficient system resources."));
			break;
		default:
			cpi_error(context, N_("Could not scan all plug-ins."));
			break;
	}
	cpi_unlock_context(context);
	
	// Release resources 
	if (pdir_path != NULL) {
		free(pdir_path);
	}
	if (avail_plugins != NULL) {
		hscan_t hscan;
		hnode_t *hnode;
		
		hash_scan_begin(&hscan, avail_plugins);
		while ((hnode = hash_scan_next(&hscan)) != NULL) {
			cp_plugin_info_t *p = hnode_get(hnode);
			hash_scan_delfree(avail_plugins, hnode);
			cp_release_info(context, p);
		}
		hash_destroy(avail_plugins);
	}
	if (started_plugins != NULL) {
		list_process(started_plugins, NULL, cpi_process_free_ptr);
		list_destroy(started_plugins);
	}
	if (plugins != NULL) {
		cp_release_info(context, plugins);
	}

	return status;
}
