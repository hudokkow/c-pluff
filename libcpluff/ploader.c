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
 * Local plug-in loader
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include "cpluff.h"
#include "defines.h"
#include "util.h"
#include "internal.h"


/* ------------------------------------------------------------------------
 * Variables
 * ----------------------------------------------------------------------*/

/// Existing local plug-in loaders
static list_t *local_ploaders = NULL;


/* ------------------------------------------------------------------------
 * Function definitions
 * ----------------------------------------------------------------------*/

static cp_plugin_info_t **lpl_scan_plugins(void *data, cp_context_t *ctx);

CP_C_API cp_plugin_loader_t *cp_create_local_ploader(cp_status_t *error) {
	cp_plugin_loader_t *loader = NULL;
	cp_status_t status = CP_OK;
	
	// Allocate and initialize a new local plug-in loader
	do {
	
		// Allocate memory for the loader
		if ((loader = malloc(sizeof(cp_plugin_loader_t))) == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
		
		// Initialize loader
		memset(loader, 0, sizeof(cp_plugin_loader_t));
		loader->data = list_create(LISTCOUNT_T_MAX);
		loader->scan_plugins = lpl_scan_plugins;
		loader->resolve_files = NULL;
		loader->release_plugins = NULL;
		if (loader->data == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
	
		// Create a local loader list, if necessary, and add loader to the list
		cpi_lock_framework();
		if (local_ploaders == NULL) {
			if ((local_ploaders = list_create(LISTCOUNT_T_MAX)) == NULL) {
				status = CP_ERR_RESOURCE;
			}
		}
		if (status == CP_OK) {
			lnode_t *node;
			
			if ((node = lnode_create(loader)) == NULL) {
				status = CP_ERR_RESOURCE;
			} else {
				list_append(local_ploaders, node);
			}
		}
		cpi_unlock_framework();
	
	} while (0);
	
	// Release resources on failure
	if (status != CP_OK) {
		if (loader != NULL) {
			cp_destroy_local_ploader(loader);
		}
		loader = NULL;
	}
	
	// Return the final status 
	if (error != NULL) {
		*error = status;
	}
	
	// Return the loader (or NULL on failure)
	return loader;	
}

CP_C_API void cp_destroy_local_ploader(cp_plugin_loader_t *loader) {
	list_t *dirs;
	
	CHECK_NOT_NULL(loader);
	
	dirs = (list_t *) loader->data;
	if (loader->data != NULL) {
		list_process(dirs, NULL, cpi_process_free_ptr);
		list_destroy(dirs);
		loader->data = NULL;
	}
	free(loader);
}

CP_C_API cp_status_t cp_lpl_register_dir(cp_plugin_loader_t *loader, const char *dir) {
	char *d = NULL;
	lnode_t *node = NULL;
	cp_status_t status = CP_OK;
	list_t *dirs;
	
	CHECK_NOT_NULL(loader);
	CHECK_NOT_NULL(dir);
	
	dirs = (list_t *) loader->data;
	do {
	
		// Check if directory has already been registered 
		if (list_find(dirs, dir, (int (*)(const void *, const void *)) strcmp) != NULL) {
			break;
		}
	
		// Allocate resources 
		d = malloc(sizeof(char) * (strlen(dir) + 1));
		node = lnode_create(d);
		if (d == NULL || node == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
	
		// Register directory 
		strcpy(d, dir);
		list_append(dirs, node);
		
	} while (0);

	// Release resources on failure 
	if (status != CP_OK) {	
		if (d != NULL) {
			free(d);
		}
		if (node != NULL) {
			lnode_destroy(node);
		}
	}
	
	return status;
}

CP_C_API void cp_lpl_unregister_dir(cp_plugin_loader_t *loader, const char *dir) {
	char *d;
	lnode_t *node;
	list_t *dirs;
	
	CHECK_NOT_NULL(loader);
	CHECK_NOT_NULL(dir);
	
	dirs = (list_t *) loader->data;
	node = list_find(dirs, dir, (int (*)(const void *, const void *)) strcmp);
	if (node != NULL) {
		d = lnode_get(node);
		list_delete(dirs, node);
		lnode_destroy(node);
		free(d);
	}
}

CP_C_API void cp_lpl_unregister_dirs(cp_plugin_loader_t *loader) {
	list_t *dirs;
	
	CHECK_NOT_NULL(loader);
	dirs = (list_t *) loader->data;
	list_process(dirs, NULL, cpi_process_free_ptr);
}

static cp_plugin_info_t **lpl_scan_plugins(void *data, cp_context_t *ctx) {
	hash_t *avail_plugins = NULL;
	char *pdir_path = NULL;
	int pdir_path_size = 0;
	list_t *dirs;
	cp_plugin_info_t **plugins = NULL;
	
	CHECK_NOT_NULL(data);
	CHECK_NOT_NULL(ctx);
	
	dirs = (list_t*) data;
	do {
		lnode_t *lnode;
		hscan_t hscan;
		hnode_t *hnode;
		int num_avail_plugins;
		int i;
	
		// Create a hash for available plug-ins 
		if ((avail_plugins = hash_create(HASHCOUNT_T_MAX, (int (*)(const void *, const void *)) strcmp, NULL)) == NULL) {
			break;
		}
	
		// Scan plug-in loaders for available plug-ins 
		lnode = list_first(dirs);
		while (lnode != NULL) {			
			const char *dir_path;
			DIR *dir;
			
			dir_path = lnode_get(lnode);
			dir = opendir(dir_path);
			if (dir != NULL) {
				int dir_path_len;
				struct dirent *de;
				
				dir_path_len = strlen(dir_path);
				if (dir_path[dir_path_len - 1] == CP_FNAMESEP_CHAR) {
					dir_path_len--;
				}
				errno = 0;
				while ((de = readdir(dir)) != NULL) {
					if (de->d_name[0] != '\0' && de->d_name[0] != '.') {
						int pdir_path_len = dir_path_len + 1 + strlen(de->d_name) + 1;
						cp_plugin_info_t *plugin;
						cp_status_t s;
						hnode_t *hnode;

						// Allocate memory for plug-in descriptor path 
						if (pdir_path_size <= pdir_path_len) {
							char *new_pdir_path;
						
							if (pdir_path_size == 0) {
								pdir_path_size = 128;
							}
							while (pdir_path_size <= pdir_path_len) {
								pdir_path_size *= 2;
							}
							new_pdir_path = realloc(pdir_path, pdir_path_size * sizeof(char));
							if (new_pdir_path == NULL) {
								cpi_errorf(ctx, N_("Could not check possible plug-in location %s%c%s due to insufficient system resources."), dir_path, CP_FNAMESEP_CHAR, de->d_name);

								// continue loading plug-ins from other directories 
								continue;
							}
							pdir_path = new_pdir_path;
						}
					
						// Construct plug-in descriptor path 
						strcpy(pdir_path, dir_path);
						pdir_path[dir_path_len] = CP_FNAMESEP_CHAR;
						strcpy(pdir_path + dir_path_len + 1, de->d_name);
							
						// Try to load a plug-in 
						plugin = cp_load_plugin_descriptor(ctx, pdir_path, &s);
						if (plugin == NULL) {
						
							// continue loading plug-ins from other directories 
							continue;
						}
					
						// Insert plug-in to the list of available plug-ins 
						if ((hnode = hash_lookup(avail_plugins, plugin->identifier)) != NULL) {
							cp_plugin_info_t *plugin2 = hnode_get(hnode);
							if (cpi_vercmp(plugin->version, plugin2->version) > 0) {
								hash_delete_free(avail_plugins, hnode);
								cp_release_info(ctx, plugin2);
								hnode = NULL;
							}
						}
						if (hnode == NULL) {
							if (!hash_alloc_insert(avail_plugins, plugin->identifier, plugin)) {
								cpi_errorf(ctx, N_("Plug-in %s version %s could not be loaded due to insufficient system resources."), plugin->identifier, plugin->version);
								cp_release_info(ctx, plugin);

								// continue loading plug-ins from other directories 
								continue;
							}
						}
						
					}
					errno = 0;
				}
				if (errno) {
					cpi_errorf(ctx, N_("Could not read plug-in directory %s: %s"), dir_path, strerror(errno));
					// continue loading plug-ins from other directories 
				}
				closedir(dir);
			} else {
				cpi_errorf(ctx, N_("Could not open plug-in directory %s: %s"), dir_path, strerror(errno));
				// continue loading plug-ins from other directories 
			}
			
			lnode = list_next(dirs, lnode);
		}

		// Construct an array of plug-ins
		num_avail_plugins = hash_count(avail_plugins);
		if ((plugins = malloc(sizeof(cp_plugin_info_t *) * (num_avail_plugins + 1))) == NULL) {
			break;
		}
		hash_scan_begin(&hscan, avail_plugins);
		i = 0;
		while ((hnode = hash_scan_next(&hscan)) != NULL) {
			cp_plugin_info_t *p = hnode_get(hnode);
			hash_scan_delfree(avail_plugins, hnode);
			plugins[i++] = p;
		}
		plugins[i++] = NULL;
		hash_destroy(avail_plugins);
		avail_plugins = NULL;

	} while (0);
	
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
			cp_release_info(ctx, p);
		}
		hash_destroy(avail_plugins);
	}
	
	return plugins;
}

CP_C_API cp_plugin_info_t * cp_load_plugin_descriptor_from_memory(cp_context_t *context, const char *buffer, unsigned int buffer_len, cp_status_t *error) {
	char *file = NULL;
  const char *path = "memory";
	cp_status_t status = CP_OK;
	XML_Parser parser = NULL;
	ploader_context_t *plcontext = NULL;
	cp_plugin_info_t *plugin = NULL;

	CHECK_NOT_NULL(context);
	CHECK_NOT_NULL(buffer);
	cpi_lock_context(context);
	cpi_check_invocation(context, CPI_CF_ANY, __func__);
	do {
		int path_len = 6;
		file = malloc((path_len + 1) * sizeof(char));
		if (file == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
    strcpy(file, path);

		// Initialize the XML parsing 
		parser = XML_ParserCreate(NULL);
		if (parser == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
		XML_SetElementHandler(parser,
			start_element_handler,
			end_element_handler);
		
		// Initialize the parsing context 
		if ((plcontext = malloc(sizeof(ploader_context_t))) == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
		memset(plcontext, 0, sizeof(ploader_context_t));
		if ((plcontext->plugin = malloc(sizeof(cp_plugin_info_t))) == NULL) {
			status = CP_ERR_RESOURCE;
			break;
		}
		plcontext->context = context;
		plcontext->configuration = NULL;
		plcontext->value = NULL;
		plcontext->parser = parser;
		plcontext->file = file;
		plcontext->state = PARSER_BEGIN;
		memset(plcontext->plugin, 0, sizeof(cp_plugin_info_t));
		plcontext->plugin->name = NULL;
		plcontext->plugin->identifier = NULL;
		plcontext->plugin->version = NULL;
		plcontext->plugin->provider_name = NULL;
		plcontext->plugin->abi_bw_compatibility = NULL;
		plcontext->plugin->api_bw_compatibility = NULL;
		plcontext->plugin->plugin_path = NULL;
		plcontext->plugin->req_cpluff_version = NULL;
		plcontext->plugin->imports = NULL;
		plcontext->plugin->runtime_lib_name = NULL;
		plcontext->plugin->runtime_funcs_symbol = NULL;
		plcontext->plugin->ext_points = NULL;
		plcontext->plugin->extensions = NULL;
		XML_SetUserData(parser, plcontext);

		// Parse the plug-in descriptor 
    do {
		  void *xml_buffer;
		  int i;
  		
		  // Get buffer from Expat 
		  if ((xml_buffer = XML_GetBuffer(parser, buffer_len))
			  == NULL) {
			  status = CP_ERR_RESOURCE;
			  break;
		  }
  		
		  // Read data into buffer
      memcpy(xml_buffer, buffer, buffer_len);

		  // Parse the data 
		  if (!(i = XML_ParseBuffer(parser, buffer_len, 1))
			  && context != NULL) {
			  cpi_lock_context(context);
			  cpi_errorf(context,
				  N_("XML parsing error in %s, line %d, column %d (%s)."),
				  file,
				  XML_GetErrorLineNumber(parser),
				  XML_GetErrorColumnNumber(parser) + 1,
				  XML_ErrorString(XML_GetErrorCode(parser)));
			  cpi_unlock_context(context);
		  }
		  if (!i || plcontext->state == PARSER_ERROR) {
			  status = CP_ERR_MALFORMED;
			  break;
		  }
    } while (0);
		if (status == CP_OK) {
			if (plcontext->state != PARSER_END || plcontext->error_count > 0) {
				status = CP_ERR_MALFORMED;
			}
			if (plcontext->resource_error_count > 0) {
				status = CP_ERR_RESOURCE;
			}
		}
		if (status != CP_OK) {
			break;
		}
    
		// Initialize the plug-in path 
		*(file + path_len) = '\0';
		plcontext->plugin->plugin_path = file;
		file = NULL;
		
		// Increase plug-in usage count
		if ((status = cpi_register_info(context, plcontext->plugin, (void (*)(cp_context_t *, void *)) dealloc_plugin_info)) != CP_OK) {
			break;
		}
		
	} while (0);

	// Report possible errors
	if (status != CP_OK) {
		switch (status) {
			case CP_ERR_MALFORMED:
				cpi_errorf(context,
					N_("Plug-in descriptor in %s is invalid."), path);
				break;
			case CP_ERR_IO:
				cpi_debugf(context,
					N_("An I/O error occurred while loading a plug-in descriptor from %s."), path);
				break;
			case CP_ERR_RESOURCE:
				cpi_errorf(context,
					N_("Insufficient system resources to load a plug-in descriptor from %s."), path);
				break;
			default:
				cpi_errorf(context,
					N_("Failed to load a plug-in descriptor from %s."), path);
				break;
		}
	}
	cpi_unlock_context(context);

	// Release persistently allocated data on failure 
	if (status != CP_OK) {
		if (file != NULL) {
			free(file);
			file = NULL;
		}
		if (plcontext != NULL && plcontext->plugin != NULL) {
			cpi_free_plugin(plcontext->plugin);
			plcontext->plugin = NULL;
		}
	}
	
	// Otherwise copy the plug-in pointer
	else {
		plugin = plcontext->plugin;
	}

	// Release data allocated for parsing 
	if (parser != NULL) {
		XML_ParserFree(parser);
	}
	if (plcontext != NULL) {
		if (plcontext->value != NULL) {
			free(plcontext->value);
		}
		free(plcontext);
		plcontext = NULL;
	}

	// Return error code
	if (error != NULL) {
		*error = status;
	}

	return plugin;
}
