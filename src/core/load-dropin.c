/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <dirent.h>
#include <errno.h>

#include "unit.h"
#include "load-dropin.h"
#include "log.h"
#include "strv.h"
#include "unit-name.h"
#include "conf-parser.h"
#include "load-fragment.h"
#include "conf-files.h"

static int iterate_dir(Unit *u, const char *path, UnitDependency dependency, char ***strv) {
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        assert(u);
        assert(path);

        /* The config directories are special, since the order of the
         * drop-ins matters */
        if (dependency < 0)  {
                r = strv_extend(strv, path);
                if (r < 0)
                        return log_oom();

                return 0;
        }

        d = opendir(path);
        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return -errno;
        }

        for (;;) {
                struct dirent *de;
                union dirent_storage buf;
                _cleanup_free_ char *f = NULL;
                int k;

                k = readdir_r(d, &buf.de, &de);
                if (k != 0) {
                        log_error("Failed to read directory %s: %s", path, strerror(k));
                        return -k;
                }

                if (!de)
                        break;

                if (ignore_file(de->d_name))
                        continue;

                f = strjoin(path, "/", de->d_name, NULL);
                if (!f)
                        return log_oom();

                r = unit_add_dependency_by_name(u, dependency, de->d_name, f, true);
                if (r < 0)
                        log_error("Cannot add dependency %s to %s, ignoring: %s", de->d_name, u->id, strerror(-r));
        }

        return 0;
}

static int process_dir(Unit *u, const char *unit_path, const char *name, const char *suffix, UnitDependency dependency, char ***strv) {
        int r;
        char *path;

        assert(u);
        assert(unit_path);
        assert(name);
        assert(suffix);

        path = strjoin(unit_path, "/", name, suffix, NULL);
        if (!path)
                return -ENOMEM;

        if (u->manager->unit_path_cache &&
            !set_get(u->manager->unit_path_cache, path))
                r = 0;
        else
                r = iterate_dir(u, path, dependency, strv);
        free(path);

        if (r < 0)
                return r;

        if (u->instance) {
                char *template;
                /* Also try the template dir */

                template = unit_name_template(name);
                if (!template)
                        return -ENOMEM;

                path = strjoin(unit_path, "/", template, suffix, NULL);
                free(template);

                if (!path)
                        return -ENOMEM;

                if (u->manager->unit_path_cache &&
                    !set_get(u->manager->unit_path_cache, path))
                        r = 0;
                else
                        r = iterate_dir(u, path, dependency, strv);
                free(path);

                if (r < 0)
                        return r;
        }

        return 0;
}

char **unit_find_dropin_paths(Unit *u) {
        Iterator i;
        char *t;
        _cleanup_strv_free_ char **strv = NULL;
        char **configs = NULL;
        int r;

        assert(u);

        SET_FOREACH(t, u->names, i) {
                char **p;

                STRV_FOREACH(p, u->manager->lookup_paths.unit_path) {
                        /* This loads the drop-in config snippets */
                        r = process_dir(u, *p, t, ".d", _UNIT_DEPENDENCY_INVALID, &strv);
                        if (r < 0)
                                return NULL;
                }
        }

        if (!strv_isempty(strv)) {
                r = conf_files_list_strv(&configs, ".conf", NULL, (const char**) strv);
                if (r < 0) {
                        log_error("Failed to get list of configuration files: %s", strerror(-r));
                        strv_free(configs);
                        return NULL;
                }

        }

        return configs;
}

int unit_load_dropin(Unit *u) {
        Iterator i;
        char *t, **f;
        int r;

        assert(u);

        /* Load dependencies from supplementary drop-in directories */

        SET_FOREACH(t, u->names, i) {
                char **p;

                STRV_FOREACH(p, u->manager->lookup_paths.unit_path) {
                        r = process_dir(u, *p, t, ".wants", UNIT_WANTS, NULL);
                        if (r < 0)
                                return r;

                        r = process_dir(u, *p, t, ".requires", UNIT_REQUIRES, NULL);
                        if (r < 0)
                                return r;
                }
        }

        u->dropin_paths = unit_find_dropin_paths(u);
        if (! u->dropin_paths)
                return 0;

        STRV_FOREACH(f, u->dropin_paths) {
                r = config_parse(u->id, *f, NULL,
                                 UNIT_VTABLE(u)->sections, config_item_perf_lookup,
                                 (void*) load_fragment_gperf_lookup, false, u);
                if (r < 0)
                        return r;
        }

        u->dropin_mtime = now(CLOCK_REALTIME);

        return 0;
}
