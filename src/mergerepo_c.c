/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include "version.h"
#include "compression_wrapper.h"
#include "misc.h"
#include "locate_metadata.h"
#include "load_metadata.h"
#include "package.h"
#include "xml_dump.h"
#include "repomd.h"
#include "sqlite.h"


// TODO:
//  - rozvijet architekturu na listy tak jak to dela mergedrepo


#define G_LOG_DOMAIN    ((gchar*) 0)

#define DEFAULT_OUTPUTDIR               "merged_repo/"
#define DEFAULT_DB_COMPRESSION_TYPE             CR_CW_BZ2_COMPRESSION
#define DEFAULT_GROUPFILE_COMPRESSION_TYPE      CR_CW_GZ_COMPRESSION

// struct KojiMergedReposStuff
// contains information needed to simulate sort_and_filter() method from
// mergerepos script from Koji.
//
// sort_and_filter() method description:
// ------------------------------------
// For each package object, check if the srpm name has ever been seen before.
// If is has not, keep the package.  If it has, check if the srpm name was first
// seen in the same repo as the current package.  If so, keep the package from
// the srpm with the highest NVR.  If not, keep the packages from the first
// srpm we found, and delete packages from all other srpms.
//
// Packages with matching NVRs in multiple repos will be taken from the first
// repo.
//
// If the srpm name appears in the blocked package list, any packages generated
// from the srpm will be deleted from the package sack as well.
//
// This method will also generate a file called "pkgorigins" and add it to the
// repo metadata. This is a tab-separated map of package E:N-V-R.A to repo URL
// (as specified on the command-line). This allows a package to be tracked back
// to its origin, even if the location field in the repodata does not match the
// original repo location.

struct srpm_val {
    int repo_id;        // id of repository
    char *sourcerpm;    // pkg->rpm_sourcerpm
};

struct KojiMergedReposStuff {
    GHashTable *blocked_srpms;
    // blocked_srpms:
    // Names of sprms which will be skipped
    //   Key: srpm name
    //   Value: NULL (not important)
    GHashTable *include_srpms;
    // include_srpms:
    // Only packages from srpms included in this table will be included
    // in output merged metadata.
    //   Key: srpm name
    //   Value: struct srpm_val
    GHashTable *seen_rpms;
    // seen_rpms:
    // List of packages already included into the output metadata.
    // Purpose of this list is to avoid a duplicit packages in output.
    //   Key: string with package n-v-r.a
    //   Value: NULL (not important)
    CR_FILE *pkgorigins;
    // Every element has format: pkg_nvra\trepourl
};

typedef enum {
    MM_DEFAULT,
    MM_REPO = MM_DEFAULT,
    MM_TIMESTAMP,
    MM_NVR
} MergeMethod;


struct CmdOptions {

    // Items filled by cmd option parser

    gboolean version;
    char **repos;
    char *archlist;
    gboolean database;
    gboolean no_database;
    gboolean verbose;
    char *outputdir;
    char *outputrepo;
    gboolean nogroups;
    gboolean noupdateinfo;
    char *compress_type;
    char *merge_method_str;
    gboolean all;
    char *noarch_repo_url;

    // Koji mergerepos specific options
    gboolean koji;
    char *groupfile;
    char *blocked;

    // Items filled by check_arguments()

    char *out_dir;
    char *out_repo;
    char *tmp_out_repo;
    GSList *repo_list;
    GSList *arch_list;
    cr_CompressionType db_compression_type;
    cr_CompressionType groupfile_compression_type;
    MergeMethod merge_method;
};


struct CmdOptions _cmd_options = {
        .db_compression_type = DEFAULT_DB_COMPRESSION_TYPE,
        .groupfile_compression_type = DEFAULT_GROUPFILE_COMPRESSION_TYPE,
        .merge_method = MM_DEFAULT
    };


static GOptionEntry cmd_entries[] =
{
    { "version", 0, 0, G_OPTION_ARG_NONE, &(_cmd_options.version),
      "Show program's version number and exit", NULL },
    { "repo", 'r', 0, G_OPTION_ARG_FILENAME_ARRAY, &(_cmd_options.repos),
      "Repo url", "REPOS" },
    { "archlist", 'a', 0, G_OPTION_ARG_STRING, &(_cmd_options.archlist),
      "Defaults to all arches - otherwise specify arches", "ARCHLIST" },
    { "database", 'd', 0, G_OPTION_ARG_NONE, &(_cmd_options.database),
      "", NULL },
    { "no-database", 0, 0, G_OPTION_ARG_NONE, &(_cmd_options.no_database),
      "", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &(_cmd_options.verbose),
      "", NULL },
    { "outputdir", 'o', 0, G_OPTION_ARG_FILENAME, &(_cmd_options.outputdir),
      "Location to create the repository", "OUTPUTDIR" },
    { "nogroups", 0, 0, G_OPTION_ARG_NONE, &(_cmd_options.nogroups),
      "Do not merge group (comps) metadata", NULL },
    { "noupdateinfo", 0, 0, G_OPTION_ARG_NONE, &(_cmd_options.noupdateinfo),
      "Do not merge updateinfo metadata", NULL },
    { "compress-type", 0, 0, G_OPTION_ARG_STRING, &(_cmd_options.compress_type),
      "Which compression type to use", "COMPRESS_TYPE" },
    { "method", 0, 0, G_OPTION_ARG_STRING, &(_cmd_options.merge_method_str),
      "Specify merge method for packages with the same name and arch (available"
      " merge methods: repo (default), ts, nvr)", "MERGE_METHOD" },
    { "all", 0, 0, G_OPTION_ARG_NONE, &(_cmd_options.all),
      "Include all packages with the same name and arch if version or release "
      "is different. If used --method argument is ignored!", NULL },
    { "noarch-repo", 0, 0, G_OPTION_ARG_FILENAME, &(_cmd_options.noarch_repo_url),
      "Packages with noarch architecture will be replaced by package from this "
      "repo if exists in it.", "URL" },
    // -- Options related to Koji-mergerepos behaviour
    { "koji", 'k', 0, G_OPTION_ARG_NONE, &(_cmd_options.koji),
       "Enable koji mergerepos behaviour.", NULL},
    { "groupfile", 'g', 0, G_OPTION_ARG_FILENAME, &(_cmd_options.groupfile),
      "Path to groupfile to include in metadata.", "GROUPFILE" },
    { "blocked", 'b', 0, G_OPTION_ARG_FILENAME, &(_cmd_options.blocked),
      "A file containing a list of srpm names to exclude from the merged repo. "
      "Only works with combination with --koji/-k.",
      "FILE" },
    // -- Options related to Koji-mergerepos behaviour - end
    { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};


GSList *
append_arch(GSList *list, gchar *arch, gboolean koji)
{
    GSList *elem;

    // Try to find arch in the list
    for (elem = list; elem; elem = g_slist_next(elem)) {
        if (!strcmp(elem->data, arch))
            return list;  // Arch already exists
    }

    list = g_slist_prepend(list, g_strdup(arch));

    if (koji) {
        // expand arch
        if (!strcmp(arch, "i386")) {
            list = append_arch(list, "i486", FALSE);
            list = append_arch(list, "i586", FALSE);
            list = append_arch(list, "geode", FALSE);
            list = append_arch(list, "i686", FALSE);
            list = append_arch(list, "athlon", FALSE);
        } else if (!strcmp(arch, "x86_64")) {
            list = append_arch(list, "ia32e", FALSE);
            list = append_arch(list, "amd64", FALSE);
        } else if (!strcmp(arch, "ppc64")) {
            list = append_arch(list, "ppc64pseries", FALSE);
            list = append_arch(list, "ppc64iseries", FALSE);
        } else if (!strcmp(arch, "sparc64")) {
            list = append_arch(list, "sparc64v", FALSE);
            list = append_arch(list, "sparc64v2", FALSE);
        } else if (!strcmp(arch, "sparc")) {
            list = append_arch(list, "sparcv8", FALSE);
            list = append_arch(list, "sparcv9", FALSE);
            list = append_arch(list, "sparcv9v", FALSE);
            list = append_arch(list, "sparcv9v2", FALSE);
        } else if (!strcmp(arch, "alpha")) {
            list = append_arch(list, "alphaev4", FALSE);
            list = append_arch(list, "alphaev45", FALSE);
            list = append_arch(list, "alphaev5", FALSE);
            list = append_arch(list, "alphaev56", FALSE);
            list = append_arch(list, "alphapca56", FALSE);
            list = append_arch(list, "alphaev6", FALSE);
            list = append_arch(list, "alphaev67", FALSE);
            list = append_arch(list, "alphaev68", FALSE);
            list = append_arch(list, "alphaev7", FALSE);
        } else if (!strcmp(arch, "armhfp")) {
            list = append_arch(list, "armv7hl", FALSE);
            list = append_arch(list, "armv7hnl", FALSE);
        } else if (!strcmp(arch, "arm")) {
            list = append_arch(list, "rmv5tel", FALSE);
            list = append_arch(list, "armv5tejl", FALSE);
            list = append_arch(list, "armv6l", FALSE);
            list = append_arch(list, "armv7l", FALSE);
        } else if (!strcmp(arch, "sh4")) {
            list = append_arch(list, "sh4a", FALSE);
        }
    }

    return list;
}


gboolean
check_arguments(struct CmdOptions *options)
{
    int x;
    gboolean ret = TRUE;

    if (options->outputdir){
        options->out_dir = cr_normalize_dir_path(options->outputdir);
    } else {
        options->out_dir = g_strdup(DEFAULT_OUTPUTDIR);
    }

    options->out_repo = g_strconcat(options->out_dir, "repodata/", NULL);
    options->tmp_out_repo = g_strconcat(options->out_dir, ".repodata/", NULL);

    // Process repos
    x = 0;
    options->repo_list = NULL;
    while (options->repos && options->repos[x] != NULL) {
        char *normalized = cr_normalize_dir_path(options->repos[x]);
        if (normalized) {
            options->repo_list = g_slist_prepend(options->repo_list, normalized);
        }
        x++;
    }
    // Reverse come with downloading repos
    // options->repo_list = g_slist_reverse (options->repo_list);

    // Process archlist
    options->arch_list = NULL;
    if (options->archlist) {
        x = 0;
        gchar **arch_set = g_strsplit_set(options->archlist, ",;", -1);
        while (arch_set && arch_set[x] != NULL) {
            gchar *arch = arch_set[x];
            if (arch[0] != '\0') {
                options->arch_list = append_arch(options->arch_list,
                                                 arch,
                                                 options->koji);
            }
            x++;
        }
        g_strfreev(arch_set);
    }

    // Compress type
    if (options->compress_type) {
        if (!g_strcmp0(options->compress_type, "gz")) {
            options->db_compression_type = CR_CW_GZ_COMPRESSION;
            options->groupfile_compression_type = CR_CW_GZ_COMPRESSION;
        } else if (!g_strcmp0(options->compress_type, "bz2")) {
            options->db_compression_type = CR_CW_BZ2_COMPRESSION;
            options->groupfile_compression_type = CR_CW_BZ2_COMPRESSION;
        } else if (!g_strcmp0(options->compress_type, "xz")) {
            options->db_compression_type = CR_CW_XZ_COMPRESSION;
            options->groupfile_compression_type = CR_CW_XZ_COMPRESSION;
        } else {
            g_critical("Compression %s not available: Please choose from: "
                       "gz or bz2 or xz", options->compress_type);
            ret = FALSE;
        }
    }

    // Merge method
    if (options->merge_method_str) {
        if (!g_strcmp0(options->merge_method_str, "repo")) {
            options->merge_method = MM_REPO;
        } else if (!g_strcmp0(options->merge_method_str, "ts")) {
            options->merge_method = MM_TIMESTAMP;
        } else if (!g_strcmp0(options->merge_method_str, "nvr")) {
            options->merge_method = MM_NVR;
        } else {
            g_critical("Unknown merge method %s", options->merge_method_str);
            ret = FALSE;
        }
    }

    // Koji arguments
    if (options->blocked) {
        if (!options->koji) {
            g_critical("-b/--blocked cannot be used without -k/--koji argument");
            ret = FALSE;
        }
        if (!g_file_test(options->blocked, G_FILE_TEST_EXISTS)) {
            g_critical("File %s doesn't exists", options->blocked);
            ret = FALSE;
        }
    }

    return ret;
}


struct CmdOptions *
parse_arguments(int *argc, char ***argv)
{
    GError *error = NULL;
    GOptionContext *context;

    context = g_option_context_new(": take 2 or more repositories and merge "
                                   "their metadata into a new repo");
    g_option_context_add_main_entries(context, cmd_entries, NULL);

    gboolean ret = g_option_context_parse(context, argc, argv, &error);
    g_option_context_free(context);
    if (!ret) {
        g_print("Option parsing failed: %s\n", error->message);
        g_error_free(error);
        return NULL;
    }

    return &(_cmd_options);
}



void
free_options(struct CmdOptions *options)
{
    g_free(options->outputdir);
    g_free(options->archlist);
    g_free(options->compress_type);
    g_free(options->merge_method_str);
    g_free(options->noarch_repo_url);

    g_free(options->groupfile);
    g_free(options->blocked);

    g_strfreev(options->repos);
    g_free(options->out_dir);
    g_free(options->out_repo);
    g_free(options->tmp_out_repo);

    GSList *element = NULL;

    // Free include_pkgs GSList
    for (element = options->repo_list; element; element = g_slist_next(element)) {
        g_free( (gchar *) element->data );
    }
    g_slist_free(options->repo_list);

    // Free include_pkgs GSList
    for (element = options->arch_list; element; element = g_slist_next(element)) {
        g_free( (gchar *) element->data );
    }
    g_slist_free(options->arch_list);
}



void
free_merged_values(gpointer data)
{
    GSList *element = (GSList *) data;
    for (; element; element=g_slist_next(element)) {
        cr_Package *pkg = (cr_Package *) element->data;
        cr_package_free(pkg);
    }
    g_slist_free((GSList *) data);
}



GHashTable *
new_merged_metadata_hashtable()
{
    GHashTable *hashtable = g_hash_table_new_full(g_str_hash,
                                                  g_str_equal,
                                                  NULL,
                                                  free_merged_values);
    return hashtable;
}



void
destroy_merged_metadata_hashtable(GHashTable *hashtable)
{
    if (hashtable) {
        g_hash_table_destroy(hashtable);
    }
}


void
cr_srpm_val_destroy(gpointer data)
{
    struct srpm_val *val = data;
    g_free(val->sourcerpm);
    g_free(val);
}


int
koji_stuff_prepare(struct KojiMergedReposStuff **koji_stuff_ptr,
                   struct CmdOptions *cmd_options,
                   GSList *repos)
{
    struct KojiMergedReposStuff *koji_stuff;
    gchar *pkgorigins_path = NULL;
    GSList *element;
    int repoid;

    // Pointers to elements in the koji_stuff_ptr
    GHashTable *blocked_srpms = NULL; // XXX
    GHashTable *include_srpms = NULL; // XXX

    koji_stuff = g_malloc0(sizeof(struct KojiMergedReposStuff));
    *koji_stuff_ptr = koji_stuff;


    // Prepare hashtables

    koji_stuff->include_srpms = g_hash_table_new_full(g_str_hash,
                                                      g_str_equal,
                                                      g_free,
                                                      cr_srpm_val_destroy);
    koji_stuff->seen_rpms = g_hash_table_new_full(g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  NULL);
    include_srpms = koji_stuff->include_srpms;

    // Load list of blocked srpm packages

    if (cmd_options->blocked) {
        int x = 0;
        char *content = NULL;
        char **names;
        GError *err;

        if (!g_file_get_contents(cmd_options->blocked, &content, NULL, &err)) {
            g_critical("Error while reading blocked file: %s", err->message);
            g_error_free(err);
            g_free(content);
            return 1;
        }

        koji_stuff->blocked_srpms = g_hash_table_new_full(g_str_hash,
                                                          g_str_equal,
                                                          g_free,
                                                          NULL);
        blocked_srpms = koji_stuff->blocked_srpms;

        names = g_strsplit(content, "\n", 0);
        while (names && names[x] != NULL) {
            if (strlen(names[x]))
                g_hash_table_replace(koji_stuff->blocked_srpms,
                                     g_strdup(names[x]),
                                     NULL);
            x++;
        }

        g_strfreev(names);
        g_free(content);
    }


    // Prepare pkgorigin file

    pkgorigins_path = g_strconcat(cmd_options->tmp_out_repo, "pkgorigins.gz", NULL);
    koji_stuff->pkgorigins = cr_open(pkgorigins_path,
                                     CR_CW_MODE_WRITE,
                                     CR_CW_GZ_COMPRESSION);
    if (!koji_stuff->pkgorigins) {
        g_critical("Cannot open file: %s", pkgorigins_path);
        g_free(pkgorigins_path);
        return 1;
    }
    g_free(pkgorigins_path);


    // Iterate over every repo and fill include_srpms hashtable

    repoid = 0;
    for (element = repos; element; element = g_slist_next(element)) {
        struct cr_MetadataLocation *ml;
        cr_Metadata metadata;
        GHashTableIter iter;
        gpointer key, value;

        metadata = cr_new_metadata(CR_HT_KEY_HASH, 0, NULL);
        ml       = (struct cr_MetadataLocation *) element->data;

        if (cr_load_xml_metadata(metadata, ml) == CR_LOAD_METADATA_ERR) {
            cr_destroy_metadata(metadata);
            g_critical("Cannot load repo: \"%s\"", ml->original_url);
            repoid++;
            break;
        }

        // Itarate over every package in repo and what "builds"
        // we're allowing into the repo
        g_hash_table_iter_init(&iter, cr_metadata_hashtable(metadata));
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            cr_Package *pkg = (cr_Package *) value;
            struct cr_NVREA *nvrea;
            gpointer data;
            gboolean blocked = FALSE;
            struct srpm_val *value_new;

            nvrea = cr_split_rpm_filename(pkg->rpm_sourcerpm);

            if (blocked_srpms) {
                // Check if srpm is blocked
                blocked = g_hash_table_lookup_extended(blocked_srpms,
                                                       nvrea->name,
                                                       NULL,
                                                       NULL);
            }

            if (blocked) {
                g_debug("Srpm is blocked: %s", pkg->rpm_sourcerpm);
                cr_nvrea_free(nvrea);
                continue;
            }

            data = g_hash_table_lookup(include_srpms, nvrea->name);
            if (data) {
                // We have already seen build with the same name

                int cmp;
                struct cr_NVREA *nvrea_other;
                struct srpm_val *value = data;

                if (value->repo_id != repoid) {
                    // We found a rpm built from an srpm with the same name in
                    // a previous repo. The previous repo takes precendence,
                    // so ignore the srpm found here.
                    cr_nvrea_free(nvrea);
                    g_debug("Srpm already loaded from previous repo %s",
                            pkg->rpm_sourcerpm);
                    continue;
                }

                // We're in the same repo, so compare srpm NVRs
                nvrea_other = cr_split_rpm_filename(value->sourcerpm);
                cmp = cr_cmp_nvrea(nvrea, nvrea_other);
                cr_nvrea_free(nvrea_other);
                if (cmp < 1) {
                    // Existing package is from the newer srpm
                    cr_nvrea_free(nvrea);
                    g_debug("Srpm already exists in newer version %s",
                            pkg->rpm_sourcerpm);
                    continue;
                }
            }

            // The current package we're processing is from a newer srpm
            // than the existing srpm in the dict, so update the dict
            // OR
            // We found a new build so we add it to the dict

            value_new = g_malloc0(sizeof(struct srpm_val));
            value_new->repo_id = repoid;
            value_new->sourcerpm = g_strdup(pkg->rpm_sourcerpm);
            g_hash_table_replace(include_srpms,
                                 g_strdup(nvrea->name),
                                 value_new);
            cr_nvrea_free(nvrea);
        }

        cr_destroy_metadata(metadata);
        repoid++;
    }


    return 0;  // All ok
}


void
koji_stuff_destroy(struct KojiMergedReposStuff **koji_stuff_ptr)
{
    struct KojiMergedReposStuff *koji_stuff;

    if (!koji_stuff_ptr || !*koji_stuff_ptr)
        return;

    koji_stuff = *koji_stuff_ptr;

    if (koji_stuff->blocked_srpms)
        g_hash_table_destroy(koji_stuff->blocked_srpms);
    g_hash_table_destroy(koji_stuff->include_srpms);
    g_hash_table_destroy(koji_stuff->seen_rpms);
    cr_close(koji_stuff->pkgorigins);
    g_free(koji_stuff);
}



// Merged table structure: {"package_name": [pkg, pkg, pkg, ...], ...}
// Return codes:
//  0 = Package was not added
//  1 = Package was added
//  2 = Package replaced old package
int
add_package(cr_Package *pkg,
            gchar *repopath,
            GHashTable *merged,
            GSList *arch_list,
            MergeMethod merge_method,
            gboolean include_all,
            struct KojiMergedReposStuff *koji_stuff)
{
    GSList *list, *element;


    // Check if the package meet the command line architecture constraints

    if (arch_list) {
        gboolean right_arch = FALSE;
        for (element=arch_list; element; element=g_slist_next(element)) {
            if (!g_strcmp0(pkg->arch, (gchar *) element->data)) {
                right_arch = TRUE;
            }
        }

        if (!right_arch) {
            g_debug("Skip - %s (Bad arch: %s)", pkg->name, pkg->arch);
            return 0;
        }
    }


    // Koji-mergerepos specific behaviour -----------------------
    if (koji_stuff) {
        struct cr_NVREA *nvrea;
        struct srpm_val *value;
        gchar *nvra;
        gboolean seen;

        // Check arch
        // TODO

        nvrea = cr_split_rpm_filename(pkg->rpm_sourcerpm);
        value = g_hash_table_lookup(koji_stuff->include_srpms, nvrea->name);
        cr_nvrea_free(nvrea);
        if (!value || g_strcmp0(pkg->rpm_sourcerpm, value->sourcerpm)) {
            // Srpm of the package is not allowed
            g_debug("Package %s has forbidden srpm %s", pkg->name,
                                                        pkg->rpm_sourcerpm);
            return 0;
        }

        nvra = cr_package_nvra(pkg);;
        seen = g_hash_table_lookup_extended(koji_stuff->seen_rpms,
                                            nvra,
                                            NULL,
                                            NULL);
        if (seen) {
            // Similar package has been already added
            g_debug("Package with same nvra (%s) has been already added",
                    nvra);
            g_free(nvra);
            return 0;
        }

        g_hash_table_replace(koji_stuff->seen_rpms, nvra, NULL);
    }
    // Koji-mergerepos specifi behaviour end --------------------

    // Lookup package in the merged

    list = (GSList *) g_hash_table_lookup(merged, pkg->name);


    // Key doesn't exist yet

    if (!list) {
        list = g_slist_prepend(list, pkg);
        if (!pkg->location_base) {
            pkg->location_base = g_string_chunk_insert(pkg->chunk, repopath);
        }
        g_hash_table_insert (merged, (gpointer) pkg->name, (gpointer) list);
        return 1;
    }


    // Check if package with the architecture isn't in the list already

    for (element=list; element; element=g_slist_next(element)) {
        cr_Package *c_pkg = (cr_Package *) element->data;
        if (!g_strcmp0(pkg->arch, c_pkg->arch)) {

            if (!include_all) {

                // Two packages has same name and arch
                // Use selected merge method to determine which package should
                // be included

                // REPO merge method
                if (merge_method == MM_REPO) {
                    // Package with the same arch already exists
                    g_debug("Package %s (%s) already exists",
                            pkg->name, pkg->arch);
                    return 0;

                // TS merge method
                } else if (merge_method == MM_TIMESTAMP) {
                    if (pkg->time_file > c_pkg->time_file) {
                        // Remove older package
                        cr_package_free(c_pkg);
                        // Replace package in element
                        if (!pkg->location_base)
                            pkg->location_base = g_string_chunk_insert(pkg->chunk,
                                                                       repopath);
                        element->data = pkg;
                        return 2;
                    } else {
                        g_debug("Newer package %s (%s) already exists",
                                pkg->name, pkg->arch);
                        return 0;
                    }

                // NVR merge method
                } else if (merge_method == MM_NVR) {
                    int cmp_res = cr_cmp_version_str(pkg->version, c_pkg->version);
                    long pkg_release   = (pkg->release)   ? strtol(pkg->release, NULL, 10)   : 0;
                    long c_pkg_release = (c_pkg->release) ? strtol(c_pkg->release, NULL, 10) : 0;

                    if (cmp_res == 1 || (cmp_res == 0 && pkg_release > c_pkg_release)) {
                        // Remove older package
                        cr_package_free(c_pkg);
                        // Replace package in element
                        if (!pkg->location_base)
                            pkg->location_base = g_string_chunk_insert(pkg->chunk, repopath);
                        element->data = pkg;
                        return 2;
                    } else {
                        g_debug("Newer version of package %s (%s) already exists",
                                pkg->name, pkg->arch);
                        return 0;
                    }
                }

            } else {

                // Two packages has same name and arch but all param is used

                int cmp_res = cr_cmp_version_str(pkg->version, c_pkg->version);
                long pkg_release   = (pkg->release)   ? strtol(pkg->release, NULL, 10)   : 0;
                long c_pkg_release = (c_pkg->release) ? strtol(c_pkg->release, NULL, 10) : 0;

                if (cmp_res == 0 && pkg_release == c_pkg_release) {
                    // Package with same name, arch, version and release
                    // is already listed
                    g_debug("Same version of package %s (%s) already exists",
                            pkg->name, pkg->arch);
                    return 0;
                }
            }
        }
    }


    // Add package

    if (!pkg->location_base) {
        pkg->location_base = g_string_chunk_insert(pkg->chunk, repopath);
    }

    // XXX: The first list element (pointed from hashtable) must stay first!
    // g_slist_append() is suitable but non effective, insert a new element
    // right after first element is optimal (at least for now)
    assert(g_slist_insert(list, pkg, 1) == list);

    return 1;
}



long
merge_repos(GHashTable *merged,
            GSList *repo_list,
            GSList *arch_list,
            MergeMethod merge_method,
            gboolean include_all,
            GHashTable *noarch_hashtable,
            struct KojiMergedReposStuff *koji_stuff)
{
    long loaded_packages = 0;
    GSList *used_noarch_keys = NULL;

    // Load all repos

    GSList *element = NULL;
    for (element = repo_list; element; element = g_slist_next(element)) {
        gchar *repopath;                    // base url of current repodata
        cr_Metadata metadata;               // current repodata
        struct cr_MetadataLocation *ml;     // location of current repodata

        metadata = cr_new_metadata(CR_HT_KEY_HASH, 0, NULL);
        ml       = (struct cr_MetadataLocation *) element->data;
        repopath = cr_normalize_dir_path(ml->original_url);

        // Base paths in output of original createrepo doesn't have trailing '/'
        if (repopath && strlen(repopath) > 1)
            repopath[strlen(repopath)-1] = '\0';

        g_debug("Processing: %s", repopath);

        if (cr_load_xml_metadata(metadata, ml) == CR_LOAD_METADATA_ERR) {
            cr_destroy_metadata(metadata);
            g_critical("Cannot load repo: \"%s\"", ml->repomd);
            break;
        }

        GHashTableIter iter;
        gpointer key, value;
        guint original_size;
        long repo_loaded_packages = 0;

        original_size = g_hash_table_size(cr_metadata_hashtable(metadata));

        g_hash_table_iter_init (&iter, cr_metadata_hashtable(metadata));
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            int ret;
            cr_Package *pkg = (cr_Package *) value;

            // Lookup a package in the noarch_hashtable
            gboolean noarch_pkg_used = FALSE;
            if (noarch_hashtable && !g_strcmp0(pkg->arch, "noarch")) {
                cr_Package *noarch_pkg;
                noarch_pkg = g_hash_table_lookup(noarch_hashtable, pkg->location_href);
                if (noarch_pkg) {
                    pkg = noarch_pkg;
                    noarch_pkg_used = TRUE;
                }
            }

            // Add package
            ret = add_package(pkg,
                              repopath,
                              merged,
                              arch_list,
                              merge_method,
                              include_all,
                              koji_stuff);

            if (ret > 0) {
                if (!noarch_pkg_used) {
                    // Original package was added
                    // => remove only record from hashtable
                    g_hash_table_iter_steal(&iter);
                } else {
                    // Package from noarch repo was added
                    // => do not remove record, just make note
                    used_noarch_keys = g_slist_prepend(used_noarch_keys,
                                                       pkg->location_href);
                    g_debug("Package: %s (from: %s) has been replaced by noarch package",
                            pkg->location_href, repopath);
                }

                if (ret == 1) {
                    repo_loaded_packages++;
                    // Koji-mergerepos specific behaviour -----------
                    if (koji_stuff && koji_stuff->pkgorigins) {
                        gchar *nvra = cr_package_nvra(pkg);
                        cr_printf(koji_stuff->pkgorigins,
                                  "%s\t%s\n",
                                  nvra, ml->original_url);
                        g_free(nvra);
                    }
                    // Koji-mergerepos specific behaviour - end -----
                }
            }
        }

        loaded_packages += repo_loaded_packages;
        cr_destroy_metadata(metadata);
        g_debug("Repo: %s (Loaded: %ld Used: %ld)", repopath,
                (unsigned long) original_size, repo_loaded_packages);
        g_free(repopath);
    }


    // Steal used keys from noarch_hashtable

    for (element = used_noarch_keys; element; element = g_slist_next(element))
        g_hash_table_steal(noarch_hashtable, (gconstpointer) element->data);
    g_slist_free(used_noarch_keys);

    return loaded_packages;
}



int
package_cmp(gconstpointer a_p, gconstpointer b_p)
{
    int ret;
    const cr_Package *pkg_a = a_p;
    const cr_Package *pkg_b = b_p;
    ret = g_strcmp0(pkg_a->location_href, pkg_b->location_href);
    if (ret) return ret;
    return g_strcmp0(pkg_a->location_base, pkg_b->location_base);
}


int
dump_merged_metadata(GHashTable *merged_hashtable,
                     long packages,
                     gchar *groupfile,
                     struct CmdOptions *cmd_options)
{
    // Create/Open output xml files

    CR_FILE *pri_f;
    CR_FILE *fil_f;
    CR_FILE *oth_f;

    const char *groupfile_suffix = cr_compression_suffix(cmd_options->groupfile_compression_type);

    gchar *pri_xml_filename = g_strconcat(cmd_options->tmp_out_repo, "/primary.xml.gz", NULL);
    gchar *fil_xml_filename = g_strconcat(cmd_options->tmp_out_repo, "/filelists.xml.gz", NULL);
    gchar *oth_xml_filename = g_strconcat(cmd_options->tmp_out_repo, "/other.xml.gz", NULL);
    gchar *update_info_filename = NULL;
    if (!cmd_options->noupdateinfo)
        update_info_filename  = g_strconcat(cmd_options->tmp_out_repo,
                                            "/updateinfo.xml",
                                            groupfile_suffix, NULL);

    if ((pri_f = cr_open(pri_xml_filename, CR_CW_MODE_WRITE, CR_CW_GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", pri_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        g_free(update_info_filename);
        return 0;
    }

    if ((fil_f = cr_open(fil_xml_filename, CR_CW_MODE_WRITE, CR_CW_GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", fil_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        g_free(update_info_filename);
        cr_close(pri_f);
        return 0;
    }

    if ((oth_f = cr_open(oth_xml_filename, CR_CW_MODE_WRITE, CR_CW_GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", oth_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        g_free(update_info_filename);
        cr_close(fil_f);
        cr_close(pri_f);
        return 0;
    }


    // Write xml headers

    cr_printf(pri_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<metadata xmlns=\""CR_XML_COMMON_NS"\" xmlns:rpm=\""
              CR_XML_RPM_NS"\" packages=\"%d\">\n", packages);
    cr_printf(fil_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<filelists xmlns=\""CR_XML_FILELISTS_NS"\" packages=\"%d\">\n",
              packages);
    cr_printf(oth_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<otherdata xmlns=\""CR_XML_OTHER_NS"\" packages=\"%d\">\n",
              packages);


    // Prepare sqlite if needed

    sqlite3 *pri_db = NULL;
    sqlite3 *fil_db = NULL;
    sqlite3 *oth_db = NULL;
    cr_DbPrimaryStatements pri_statements = NULL;
    cr_DbFilelistsStatements fil_statements = NULL;
    cr_DbOtherStatements oth_statements = NULL;

    if (!cmd_options->no_database) {
        gchar *pri_db_filename = NULL;
        gchar *fil_db_filename = NULL;
        gchar *oth_db_filename = NULL;

        pri_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/primary.sqlite", NULL);
        fil_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/filelists.sqlite", NULL);
        oth_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/other.sqlite", NULL);

        pri_db = cr_open_primary_db(pri_db_filename, NULL);
        fil_db = cr_open_filelists_db(fil_db_filename, NULL);
        oth_db = cr_open_other_db(oth_db_filename, NULL);

        g_free(pri_db_filename);
        g_free(fil_db_filename);
        g_free(oth_db_filename);

        pri_statements = cr_prepare_primary_db_statements(pri_db, NULL);
        fil_statements = cr_prepare_filelists_db_statements(fil_db, NULL);
        oth_statements = cr_prepare_other_db_statements(oth_db, NULL);
    }


    // Dump hashtable

    GList *keys, *key;
    keys = g_hash_table_get_keys(merged_hashtable);
    keys = g_list_sort(keys, (GCompareFunc) g_strcmp0);

    for (key = keys; key; key = g_list_next(key)) {
        gpointer value = g_hash_table_lookup(merged_hashtable, key->data);
        GSList *element = (GSList *) value;
        element = g_slist_sort(element, package_cmp);
        for (; element; element=g_slist_next(element)) {
            struct cr_XmlStruct res;
            cr_Package *pkg;

            pkg = (cr_Package *) element->data;
            res = cr_xml_dump(pkg);

            cr_puts(pri_f, (const char *) res.primary);
            cr_puts(fil_f, (const char *) res.filelists);
            cr_puts(oth_f, (const char *) res.other);

            if (!cmd_options->no_database) {
                cr_add_primary_pkg_db(pri_statements, pkg);
                cr_add_filelists_pkg_db(fil_statements, pkg);
                cr_add_other_pkg_db(oth_statements, pkg);
            }

            free(res.primary);
            free(res.filelists);
            free(res.other);
        }
    }

    g_list_free(keys);

    // Write xml footers

    cr_puts(pri_f, "</metadata>");
    cr_puts(fil_f, "</filelists>");
    cr_puts(oth_f, "</otherdata>");


    // Close files

    cr_close(pri_f);
    cr_close(fil_f);
    cr_close(oth_f);


    // Write updateinfo.xml
    // TODO

    if (!cmd_options->noupdateinfo) {
        CR_FILE *update_info = NULL;
        if ((update_info = cr_open(update_info_filename, CR_CW_MODE_WRITE, cmd_options->groupfile_compression_type))) {
            cr_puts(update_info, "<?xml version=\"1.0\"?>\n<updates></updates>\n");
            cr_close(update_info);
        } else {
            g_warning("Cannot open file: %s", update_info_filename);
        }
    }




    // Prepare repomd records

    cr_RepomdRecord pri_xml_rec = cr_new_repomdrecord(pri_xml_filename);
    cr_RepomdRecord fil_xml_rec = cr_new_repomdrecord(fil_xml_filename);
    cr_RepomdRecord oth_xml_rec = cr_new_repomdrecord(oth_xml_filename);
    cr_RepomdRecord pri_db_rec               = NULL;
    cr_RepomdRecord fil_db_rec               = NULL;
    cr_RepomdRecord oth_db_rec               = NULL;
    cr_RepomdRecord groupfile_rec            = NULL;
    cr_RepomdRecord compressed_groupfile_rec = NULL;
    cr_RepomdRecord update_info_rec          = NULL;
    cr_RepomdRecord pkgorigins_rec           = NULL;


    // XML

    cr_fill_repomdrecord(pri_xml_rec, CR_CHECKSUM_SHA256);
    cr_fill_repomdrecord(fil_xml_rec, CR_CHECKSUM_SHA256);
    cr_fill_repomdrecord(oth_xml_rec, CR_CHECKSUM_SHA256);


    // Groupfile

    if (groupfile) {
        groupfile_rec = cr_new_repomdrecord(groupfile);
        compressed_groupfile_rec = cr_new_repomdrecord(groupfile);
        cr_process_groupfile_repomdrecord(groupfile_rec,
                                          compressed_groupfile_rec,
                                          CR_CHECKSUM_SHA256,
                                          cmd_options->groupfile_compression_type);
    }


    // Update info

    if (!cmd_options->noupdateinfo) {
        update_info_rec = cr_new_repomdrecord(update_info_filename);
        cr_fill_repomdrecord(update_info_rec, CR_CHECKSUM_SHA256);
    }


    // Pkgorigins

    if (cmd_options->koji) {
        gchar *pkgorigins_path = g_strconcat(cmd_options->tmp_out_repo, "pkgorigins.gz", NULL);
        pkgorigins_rec = cr_new_repomdrecord(pkgorigins_path);
        cr_fill_repomdrecord(pkgorigins_rec, CR_CHECKSUM_SHA256);
        g_free(pkgorigins_path);
    }


    // Sqlite db

    if (!cmd_options->no_database) {
        const char *db_suffix = cr_compression_suffix(cmd_options->db_compression_type);

        // Insert XML checksums into the dbs
        cr_dbinfo_update(pri_db, pri_xml_rec->checksum, NULL);
        cr_dbinfo_update(fil_db, fil_xml_rec->checksum, NULL);
        cr_dbinfo_update(oth_db, oth_xml_rec->checksum, NULL);

        // Close dbs
        cr_destroy_primary_db_statements(pri_statements);
        cr_destroy_filelists_db_statements(fil_statements);
        cr_destroy_other_db_statements(oth_statements);

        cr_close_primary_db(pri_db, NULL);
        cr_close_filelists_db(fil_db, NULL);
        cr_close_other_db(oth_db, NULL);

        // Compress dbs
        gchar *pri_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/primary.sqlite", NULL);
        gchar *fil_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/filelists.sqlite", NULL);
        gchar *oth_db_filename = g_strconcat(cmd_options->tmp_out_repo, "/other.sqlite", NULL);

        gchar *pri_db_c_filename = g_strconcat(pri_db_filename, db_suffix, NULL);
        gchar *fil_db_c_filename = g_strconcat(fil_db_filename, db_suffix, NULL);
        gchar *oth_db_c_filename = g_strconcat(oth_db_filename, db_suffix, NULL);

        cr_compress_file(pri_db_filename, NULL, cmd_options->db_compression_type);
        cr_compress_file(fil_db_filename, NULL, cmd_options->db_compression_type);
        cr_compress_file(oth_db_filename, NULL, cmd_options->db_compression_type);

        remove(pri_db_filename);
        remove(fil_db_filename);
        remove(oth_db_filename);

        // Prepare repomd records
        pri_db_rec = cr_new_repomdrecord(pri_db_c_filename);
        fil_db_rec = cr_new_repomdrecord(fil_db_c_filename);
        oth_db_rec = cr_new_repomdrecord(oth_db_c_filename);

        g_free(pri_db_filename);
        g_free(fil_db_filename);
        g_free(oth_db_filename);

        g_free(pri_db_c_filename);
        g_free(fil_db_c_filename);
        g_free(oth_db_c_filename);

        cr_fill_repomdrecord(pri_db_rec, CR_CHECKSUM_SHA256);
        cr_fill_repomdrecord(fil_db_rec, CR_CHECKSUM_SHA256);
        cr_fill_repomdrecord(oth_db_rec, CR_CHECKSUM_SHA256);
    }


    // Add checksums into files names

    cr_rename_repomdrecord_file(pri_xml_rec);
    cr_rename_repomdrecord_file(fil_xml_rec);
    cr_rename_repomdrecord_file(oth_xml_rec);
    cr_rename_repomdrecord_file(pri_db_rec);
    cr_rename_repomdrecord_file(fil_db_rec);
    cr_rename_repomdrecord_file(oth_db_rec);
    cr_rename_repomdrecord_file(groupfile_rec);
    cr_rename_repomdrecord_file(compressed_groupfile_rec);
    cr_rename_repomdrecord_file(update_info_rec);
    cr_rename_repomdrecord_file(pkgorigins_rec);


    // Gen repomd.xml content

    cr_Repomd repomd_obj = cr_new_repomd();
    cr_repomd_set_record(repomd_obj, pri_xml_rec,   CR_MD_PRIMARY_XML);
    cr_repomd_set_record(repomd_obj, fil_xml_rec,   CR_MD_FILELISTS_XML);
    cr_repomd_set_record(repomd_obj, oth_xml_rec,   CR_MD_OTHER_XML);
    cr_repomd_set_record(repomd_obj, pri_db_rec,    CR_MD_PRIMARY_SQLITE);
    cr_repomd_set_record(repomd_obj, fil_db_rec,    CR_MD_FILELISTS_SQLITE);
    cr_repomd_set_record(repomd_obj, oth_db_rec,    CR_MD_OTHER_SQLITE);
    cr_repomd_set_record(repomd_obj, groupfile_rec, CR_MD_GROUPFILE);
    cr_repomd_set_record(repomd_obj, compressed_groupfile_rec,
                         CR_MD_COMPRESSED_GROUPFILE);
    cr_repomd_set_record(repomd_obj, update_info_rec, CR_MD_UPDATEINFO);
    cr_repomd_set_record(repomd_obj, pkgorigins_rec, CR_MD_PKGORIGINS);

    char *repomd_xml = cr_generate_repomd_xml(repomd_obj);

    cr_free_repomd(repomd_obj);

    if (repomd_xml) {
        gchar *repomd_path = g_strconcat(cmd_options->tmp_out_repo, "repomd.xml", NULL);
        FILE *frepomd = fopen(repomd_path, "w");
        if (frepomd) {
            fputs(repomd_xml, frepomd);
            fclose(frepomd);
        } else
            g_critical("Cannot open file: %s", repomd_path);
        g_free(repomd_path);
    } else
        g_critical("Generate of repomd.xml failed");


    // Move files from out_repo into tmp_out_repo

    g_debug("Moving data from %s", cmd_options->out_repo);
    if (g_file_test(cmd_options->out_repo, G_FILE_TEST_EXISTS)) {

        // Delete old metadata
        g_debug("Removing old metadata from %s", cmd_options->out_repo);
        cr_remove_metadata_classic(cmd_options->out_dir, 0);

        // Move files from out_repo to tmp_out_repo
        GDir *dirp;
        dirp = g_dir_open (cmd_options->out_repo, 0, NULL);
        if (!dirp) {
            g_critical("Cannot open directory: %s", cmd_options->out_repo);
            exit(1);
        }

        const gchar *filename;
        while ((filename = g_dir_read_name(dirp))) {
            gchar *full_path = g_strconcat(cmd_options->out_repo, filename, NULL);
            gchar *new_full_path = g_strconcat(cmd_options->tmp_out_repo, filename, NULL);

            // Do not override new file with the old one
            if (g_file_test(new_full_path, G_FILE_TEST_EXISTS)) {
                g_debug("Skip move of: %s -> %s (the destination file already exists)",
                        full_path, new_full_path);
                g_debug("Removing: %s", full_path);
                g_remove(full_path);
                g_free(full_path);
                g_free(new_full_path);
                continue;
            }

            if (g_rename(full_path, new_full_path) == -1)
                g_critical("Cannot move file %s -> %s", full_path, new_full_path);
            else
                g_debug("Moved %s -> %s", full_path, new_full_path);

            g_free(full_path);
            g_free(new_full_path);
        }

        g_dir_close(dirp);

        // Remove out_repo
        if (g_rmdir(cmd_options->out_repo) == -1) {
            g_critical("Cannot remove %s", cmd_options->out_repo);
        } else {
            g_debug("Old out repo %s removed", cmd_options->out_repo);
        }
    }


    // Rename tmp_out_repo to out_repo
    if (g_rename(cmd_options->tmp_out_repo, cmd_options->out_repo) == -1) {
        g_critical("Cannot rename %s -> %s", cmd_options->tmp_out_repo, cmd_options->out_repo);
    } else {
        g_debug("Renamed %s -> %s", cmd_options->tmp_out_repo, cmd_options->out_repo);
    }


    // Clean up

    g_free(repomd_xml);

    g_free(pri_xml_filename);
    g_free(fil_xml_filename);
    g_free(oth_xml_filename);
    g_free(update_info_filename);


    return 1;
}



int
main(int argc, char **argv)
{
    // Parse arguments

    struct CmdOptions *cmd_options;
    cmd_options = parse_arguments(&argc, &argv);
    if (!cmd_options) {
        return 1;
    }


    // Check arguments

    if (!check_arguments(cmd_options)) {
        free_options(cmd_options);
        return 1;
    }

    if (cmd_options->version) {
        printf("Version: %d.%d.%d\n", CR_MAJOR_VERSION,
                                      CR_MINOR_VERSION,
                                      CR_PATCH_VERSION);
        free_options(cmd_options);
        exit(0);
    }

    if (g_slist_length(cmd_options->repo_list) < 2) {
        free_options(cmd_options);
        fprintf(stderr, "Usage: %s [options]\n\n"
                        "%s: take 2 or more repositories and merge their "
                        "metadata into a new repo\n\n",
                        cr_get_filename(argv[0]), cr_get_filename(argv[0]));
        return 1;
    }


    // Set logging

    g_log_set_default_handler (cr_log_fn, NULL);

    if (cmd_options->verbose) {
        // Verbose mode
        GLogLevelFlags levels = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_WARNING;
        g_log_set_handler(NULL, levels, cr_log_fn, NULL);
        g_log_set_handler("C_CREATEREPOLIB", levels, cr_log_fn, NULL);
    } else {
        // Standard mode
        GLogLevelFlags levels = G_LOG_LEVEL_DEBUG;
        g_log_set_handler(NULL, levels, cr_null_log_fn, NULL);
        g_log_set_handler("C_CREATEREPOLIB", levels, cr_null_log_fn, NULL);
    }


    // Prepare out_repo

    if (g_file_test(cmd_options->tmp_out_repo, G_FILE_TEST_EXISTS)) {
        g_critical("Temporary repodata directory: %s already exists! ("
                    "Another createrepo process is running?)", cmd_options->tmp_out_repo);
        free_options(cmd_options);
        return 1;
    }

    if (g_mkdir_with_parents (cmd_options->tmp_out_repo, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
        g_critical("Error while creating temporary repodata directory %s: %s",
                    cmd_options->tmp_out_repo, strerror(errno));

        free_options(cmd_options);
        return 1;
    }


    // Download repos

    GSList *local_repos = NULL;
    GSList *element = NULL;
    gchar *groupfile = NULL;
    gboolean cr_download_failed = FALSE;

    for (element = cmd_options->repo_list; element; element = g_slist_next(element)) {
        struct cr_MetadataLocation *loc = cr_get_metadata_location((gchar *) element->data, 1);
        if (!loc) {
            g_warning("Downloading of repodata failed: %s", (gchar *) element->data);
            cr_download_failed = TRUE;
            break;
        }
        local_repos = g_slist_prepend(local_repos, loc);
    }

    if (cr_download_failed) {
        // Remove downloaded metadata and free structures
        for (element = local_repos; element; element = g_slist_next(element)) {
            struct cr_MetadataLocation *loc = (struct cr_MetadataLocation  *) element->data;
            cr_free_metadata_location(loc);
        }
        return 1;
    }


    // Groupfile
    // XXX: There must be a better logic

    if (!cmd_options->groupfile) {
        // Use first groupfile you find
        for (element = local_repos; element; element = g_slist_next(element)) {
            struct cr_MetadataLocation *loc;
            loc = (struct cr_MetadataLocation  *) element->data;
            if (!groupfile && loc->groupfile_href) {
                if (cr_copy_file(loc->groupfile_href, cmd_options->tmp_out_repo) == CR_COPY_OK) {
                    groupfile = g_strconcat(cmd_options->tmp_out_repo,
                                            cr_get_filename(loc->groupfile_href),
                                            NULL);
                    g_debug("Using groupfile: %s", groupfile);
                    break;
                }
            }
        }
    } else {
        // Use groupfile specified by user
        if (cr_copy_file(cmd_options->groupfile, cmd_options->tmp_out_repo) == CR_COPY_OK) {
            groupfile = g_strconcat(cmd_options->tmp_out_repo,
                                    cr_get_filename(cmd_options->groupfile),
                                    NULL);
            g_debug("Using user specified groupfile: %s", groupfile);
        } else
            g_warning("Cannot copy groupfile %s", cmd_options->groupfile);
    }

    // Load noarch repo

    cr_Metadata noarch_metadata = NULL;
    // noarch_metadata->ht:
    //   Key: CR_HT_KEY_FILENAME aka pkg->location_href
    //   Value: package

    if (cmd_options->noarch_repo_url) {
        struct cr_MetadataLocation *noarch_ml;

        noarch_ml = cr_get_metadata_location(cmd_options->noarch_repo_url, 1);
        noarch_metadata = cr_new_metadata(CR_HT_KEY_FILENAME, 0, NULL);

        // Base paths in output of original createrepo doesn't have trailing '/'
        gchar *noarch_repopath = cr_normalize_dir_path(noarch_ml->original_url);
        if (noarch_repopath && strlen(noarch_repopath) > 1) {
            noarch_repopath[strlen(noarch_repopath)-1] = '\0';
        }

        g_debug("Loading noarch_repo: %s", noarch_repopath);

        if (cr_load_xml_metadata(noarch_metadata, noarch_ml) == CR_LOAD_METADATA_ERR) {
            g_error("Cannot load noarch repo: \"%s\"", noarch_ml->repomd);
            cr_destroy_metadata(noarch_metadata);
            // TODO cleanup
            cr_free_metadata_location(noarch_ml);
            return 1;
        }

        // Fill basepath - set proper base path for all packages in noarch hastable
        GHashTableIter iter;
        gpointer p_key, p_value;

        g_hash_table_iter_init (&iter, cr_metadata_hashtable(noarch_metadata));
        while (g_hash_table_iter_next (&iter, &p_key, &p_value)) {
            cr_Package *pkg = (cr_Package *) p_value;
            if (!pkg->location_base)
                pkg->location_base = g_string_chunk_insert(pkg->chunk,
                                                           noarch_repopath);
        }

        g_free(noarch_repopath);
        cr_free_metadata_location(noarch_ml);
    }


    // Prepare Koji stuff if needed

    struct KojiMergedReposStuff *koji_stuff = NULL;
    if (cmd_options->koji)
        koji_stuff_prepare(&koji_stuff, cmd_options, local_repos);


    // Load metadata

    long loaded_packages;
    GHashTable *merged_hashtable = new_merged_metadata_hashtable();
    // merged_hashtable:
    //   Key: pkg->name
    //   Value: GSList with packages with the same name

    loaded_packages = merge_repos(merged_hashtable,
                                  local_repos,
                                  cmd_options->arch_list,
                                  cmd_options->merge_method,
                                  cmd_options->all,
                                  noarch_metadata ?
                                        cr_metadata_hashtable(noarch_metadata)
                                      : NULL,
                                  koji_stuff);

    // Destroy koji stuff - we have to close pkgorigins file before dump

    if (cmd_options->koji)
        koji_stuff_destroy(&koji_stuff);


    // Dump metadata

    dump_merged_metadata(merged_hashtable, loaded_packages, groupfile, cmd_options);


    // Remove downloaded repos and free repo location structures

    for (element = local_repos; element; element = g_slist_next(element)) {
        struct cr_MetadataLocation *loc = (struct cr_MetadataLocation  *) element->data;
        cr_free_metadata_location(loc);
    }

    g_slist_free (local_repos);

    // Cleanup

    g_free(groupfile);
    cr_destroy_metadata(noarch_metadata);
    destroy_merged_metadata_hashtable(merged_hashtable);
    free_options(cmd_options);
    return 0;
}
