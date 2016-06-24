/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>

#include <android-base/stringprintf.h>
#include <selinux/selinux.h>

#include "ueventd.h"
#include "log.h"
#include "util.h"
#include "devices.h"
#include "ueventd_parser.h"
#include "property_service.h"

int ueventd_main(int argc, char **argv)
{
    /*
     * init sets the umask to 077 for forked processes. We need to
     * create files with exact permissions, without modification by
     * the umask.
     */
    umask(000);

    /* Prevent fire-and-forget children from becoming zombies.
     * If we should need to wait() for some children in the future
     * (as opposed to none right now), double-forking here instead
     * of ignoring SIGCHLD may be the better solution.
     */
    signal(SIGCHLD, SIG_IGN);

    open_devnull_stdio();
    InitKernelLogging(argv);

    LOG(INFO) << "ueventd started!";

    selinux_callback cb;
    cb.func_log = selinux_klog_callback;
    selinux_set_callback(SELINUX_CB_LOG, cb);

    std::string hardware = property_get("ro.hardware");

    ueventd_parse_config_file("/ueventd.rc");
    ueventd_parse_config_file(android::base::StringPrintf("/ueventd.%s.rc", hardware.c_str()).c_str());

    device_init();

    pollfd ufd;
    ufd.events = POLLIN;
    ufd.fd = get_device_fd();

    while (true) {
        ufd.revents = 0;
        int nr = poll(&ufd, 1, -1);
        if (nr <= 0) {
            continue;
        }
        if (ufd.revents & POLLIN) {
            handle_device_fd();
        }
    }

    return 0;
}

void set_device_permission(int nargs, char **args)
{
    char *name;
    char *attr = 0;
    mode_t perm;
    uid_t uid;
    gid_t gid;
    int prefix = 0;
    int wildcard = 0;
    char *endptr;
    char *tmp = 0;

    if (nargs == 0)
        return;

    if (args[0][0] == '#')
        return;

    name = args[0];

    if (!strncmp(name,"/sys/", 5) && (nargs == 5)) {
        LOG(INFO) << "/sys/ rule " << args[0] << " " << args[1];
        attr = args[1];
        args++;
        nargs--;
    }

    if (nargs != 4) {
        LOG(ERROR) << "invalid line ueventd.rc line for '" << args[0] << "'";
        return;
    }

    int len = strlen(name);
    char *wildcard_chr = strchr(name, '*');
    if ((name[len - 1] == '*') && (wildcard_chr == (name + len - 1))) {
        prefix = 1;
        name[len - 1] = '\0';
    } else if (wildcard_chr) {
        wildcard = 1;
    }

    perm = strtol(args[1], &endptr, 8);
    if (!endptr || *endptr != '\0') {
        LOG(ERROR) << "invalid mode '" << args[1] << "'";
        free(tmp);
        return;
    }

    struct passwd* pwd = getpwnam(args[2]);
    if (!pwd) {
        LOG(ERROR) << "invalid uid '" << args[2] << "'";
        free(tmp);
        return;
    }
    uid = pwd->pw_uid;

    struct group* grp = getgrnam(args[3]);
    if (!grp) {
        LOG(ERROR) << "invalid gid '" << args[3] << "'";
        free(tmp);
        return;
    }
    gid = grp->gr_gid;

    add_dev_perms(name, attr, perm, uid, gid, prefix, wildcard);
    free(tmp);
}
