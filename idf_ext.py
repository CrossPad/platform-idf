# SPDX-License-Identifier: GPL-3.0-or-later
"""CrossPad idf.py extension — app package manager commands.

Adds: idf.py app-list, app-install, app-remove, app-update, app-sync,
app-manage, app-status, app-track, app-backup, app-restore, app-config,
app-profile, app-device, app-new, board
"""

import os
import sys


def action_extensions(base_actions, project_path=os.getcwd()):
    # Add tools/ to path so we can import app_manager
    tools_dir = os.path.join(project_path, "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)

    def app_list_cmd(action, ctx, args, **kwargs):
        from app_manager import AppManager
        mgr = AppManager(args.project_dir)
        mgr.list_apps(show_all=kwargs.get("all", False))

    def app_install_cmd(action, ctx, args, app=None, ref=None, origin=None, force=False, **kwargs):
        from app_manager import AppManager
        if not app:
            print("Usage: idf.py app-install --app <name> [--ref <branch/tag/commit>] [--origin <repo_url>]")
            return
        mgr = AppManager(args.project_dir)
        mgr.install(app, ref=ref or "main", origin=origin, force=force)

    def app_remove_cmd(action, ctx, args, app=None, **kwargs):
        from app_manager import AppManager
        if not app:
            print("Usage: idf.py app-remove --app <name>")
            return
        mgr = AppManager(args.project_dir)
        mgr.remove(app)

    def app_update_cmd(action, ctx, args, app=None, **kwargs):
        from app_manager import AppManager
        mgr = AppManager(args.project_dir)
        mgr.update(app_name=app, update_all=kwargs.get("all", False),
                   force=kwargs.get("force", False),
                   dry_run=kwargs.get("dry_run", False))

    def app_sync_cmd(action, ctx, args, **kwargs):
        from app_manager import AppManager
        mgr = AppManager(args.project_dir)
        mgr.sync()

    def app_manage_cmd(action, ctx, args, **kwargs):
        from app_manager import core, ESP_IDF_CONFIG
        core.tui_main(ESP_IDF_CONFIG)

    def board_cmd(action, ctx, args, board=None, device=None, **kwargs):
        import crosspad_board
        argv = []
        if kwargs.get("set"):
            argv += ["--set", kwargs["set"]]
        if board:
            argv += ["--board", board]
        if device:
            argv += ["--device", device]
        rc = crosspad_board.main(argv)
        if rc:
            raise SystemExit(rc)

    def app_new_cmd(action, ctx, args, app=None, name=None, description=None,
                    category=None, **kwargs):
        from app_manager import AppManager
        if not app:
            print("Usage: idf.py app-new --app <id> [--name <name>] "
                  "[--private|--public]")
            return
        visibility = ("private" if kwargs.get("private") else
                      "public" if kwargs.get("public") else None)
        mgr = AppManager(args.project_dir)
        res = mgr.new_app(app, name=name or "", description=description or "",
                          category=category or "tools", visibility=visibility,
                          owner=kwargs.get("owner") or "")
        if not res["ok"]:
            print(f"Error: {res['error']}")
            return
        print(f"\n  {app} ready at {res['path']}")
        if res.get("repo"):
            print(f"  repo: {res['repo']}")
        import crosspad_board
        d = crosspad_board.resolve()
        args_s = " ".join(d.idf_args()) if d.rev else "-B build_v2 -DSDKCONFIG=sdkconfig.v2"
        print(f"\n  Next: idf.py {args_s} fullclean && idf.py {args_s} build")

    def app_device_cmd(action, ctx, args, **kwargs):
        from app_manager import AppManager
        mgr = AppManager(args.project_dir)
        report = mgr.device_diff()
        if not report.get("ok"):
            print(f"Device: {report.get('error')}")
            return
        print(f"\n  Device on {report['port']}\n")
        print(f"  {'component':<24}{'device':<12}{'repo':<12}ref")
        for row in report["rows"]:
            mark = " " if row["match"] else "!"
            print(f"{mark} {row['component']:<24}{row['device_commit']:<12}"
                  f"{row['local_commit']:<12}{row['device_ref']}"
                  f"{' *' if row['device_dirty'] else ''}")
        stale = report.get("stale", [])
        print(f"\n  {len(stale)} component(s) differ from this checkout\n"
              if stale else "\n  Device matches this checkout.\n")

    def app_status_cmd(action, ctx, args, app=None, **kwargs):
        from app_manager import AppManager
        AppManager(args.project_dir).status(app)

    def app_track_cmd(action, ctx, args, app=None, mode=None, ref=None,
                      commit=None, **kwargs):
        from app_manager import AppManager, core
        if not app or not mode:
            print("Usage: idf.py app-track --app <name> --mode "
                  "release|development|version|mine [--ref <branch>]")
            return
        mgr = AppManager(args.project_dir)
        mgr.ensure_config(quiet=True)
        mgr.set_app_policy(app, mode, ref=ref, commit=commit,
                           local=kwargs.get("local", False))
        print(f"{app}: {mode}{' ref=' + ref if ref else ''}")

    def app_backup_cmd(action, ctx, args, app=None, **kwargs):
        from app_manager import AppManager
        if not app:
            print("Usage: idf.py app-backup --app <name>")
            return
        dest = AppManager(args.project_dir).backup_app(app)
        print(f"Backup: {dest}" if dest else f"Nothing to back up for '{app}'.")

    def app_restore_cmd(action, ctx, args, app=None, stamp=None, **kwargs):
        from app_manager import AppManager
        mgr = AppManager(args.project_dir)
        if not app:
            print("Usage: idf.py app-restore --app <name> [--stamp <ts>]")
            return
        if kwargs.get("list", False):
            stamps = mgr.list_backups(app)
            print("\n".join(f"  {t}" for t in stamps) if stamps
                  else f"No backups for '{app}'.")
            return
        mgr.restore_backup(app, stamp)

    def app_config_cmd(action, ctx, args, flag=None, value=None, **kwargs):
        from app_manager import AppManager, core
        mgr = AppManager(args.project_dir)
        import argparse
        ns = argparse.Namespace(flag=flag, value=value,
                                local=kwargs.get("local", False),
                                gen=kwargs.get("gen", False))
        core._config_cli(mgr, ns)

    def app_profile_cmd(action, ctx, args, name=None, **kwargs):
        from app_manager import AppManager, core
        import argparse
        ns = argparse.Namespace(
            action=kwargs.get("profile_action") or "list", name=name,
            remove_extra=kwargs.get("remove_extra", False),
            description=kwargs.get("description", "") or "")
        core._profile_cli(AppManager(args.project_dir), ns)

    return {
        "actions": {
            "app-list": {
                "callback": app_list_cmd,
                "help": "List available and installed CrossPad apps.",
                "options": [
                    {
                        "names": ["--all"],
                        "is_flag": True,
                        "default": False,
                        "help": "Show all apps including incompatible platforms.",
                    },
                ],
            },
            "app-install": {
                "callback": app_install_cmd,
                "help": "Install a CrossPad app (adds git submodule to components/).",
                "options": [
                    {
                        "names": ["--app"],
                        "help": "App name to install (e.g. sampler, sequencer).",
                        "default": None,
                    },
                    {
                        "names": ["--ref"],
                        "help": "Branch, tag, or commit to checkout (default: main).",
                        "default": None,
                    },
                    {
                        "names": ["--origin"],
                        "help": "Override repository URL from registry.",
                        "default": None,
                    },
                    {
                        "names": ["--force"],
                        "is_flag": True,
                        "default": False,
                        "help": "Install even if not compatible with current platform.",
                    },
                ],
            },
            "app-remove": {
                "callback": app_remove_cmd,
                "help": "Remove a CrossPad app (removes git submodule).",
                "options": [
                    {
                        "names": ["--app"],
                        "help": "App name to remove.",
                        "default": None,
                    },
                ],
            },
            "app-new": {
                "callback": app_new_cmd,
                "help": "Scaffold a new app from the CrossPad template "
                        "(optionally creating the GitHub repo).",
                "options": [
                    {"names": ["--app"], "help": "App id (lowercase-with-dashes).",
                     "default": None},
                    {"names": ["--name"], "help": "Display name.", "default": None},
                    {"names": ["--description"], "help": "One-line description.",
                     "default": None},
                    {"names": ["--category"],
                     "help": "music | audio | tools | other", "default": None},
                    {"names": ["--private"], "is_flag": True, "default": False,
                     "help": "Create a private GitHub repo and push."},
                    {"names": ["--public"], "is_flag": True, "default": False,
                     "help": "Create a public GitHub repo and push."},
                    {"names": ["--owner"], "default": None,
                     "help": "GitHub owner (default: the gh account)."},
                ],
            },
            "app-device": {
                "callback": app_device_cmd,
                "help": "Ask the connected device what it was built from "
                        "(APP_VERSIONS) and diff against this checkout.",
            },
            "app-status": {
                "callback": app_status_cmd,
                "help": "Show track policy vs actual git state per app.",
                "options": [
                    {"names": ["--app"], "help": "Limit to one app.",
                     "default": None},
                ],
            },
            "app-track": {
                "callback": app_track_cmd,
                "help": "Set what an app follows: the latest release, development, "
                        "a version, or your own copy.",
                "options": [
                    {"names": ["--app"], "help": "App name.", "default": None},
                    {"names": ["--mode"],
                     "help": "release | development | version | mine "
                             "(old names registry | branch | pinned | local work too)",
                     "default": None},
                    {"names": ["--ref"], "help": "Branch for --mode development.",
                     "default": None},
                    {"names": ["--commit"], "help": "Commit for --mode version.",
                     "default": None},
                    {"names": ["--local"], "is_flag": True, "default": False,
                     "help": "Write to crosspad.local.json instead."},
                ],
            },
            "app-backup": {
                "callback": app_backup_cmd,
                "help": "Snapshot an app's local work into .crosspad/backups/.",
                "options": [
                    {"names": ["--app"], "help": "App name.", "default": None},
                ],
            },
            "app-restore": {
                "callback": app_restore_cmd,
                "help": "Restore an app backup.",
                "options": [
                    {"names": ["--app"], "help": "App name.", "default": None},
                    {"names": ["--stamp"], "help": "Backup timestamp.",
                     "default": None},
                    {"names": ["--list"], "is_flag": True, "default": False,
                     "help": "List available backups."},
                ],
            },
            "app-config": {
                "callback": app_config_cmd,
                "help": "Show or set compile-time feature flags.",
                "options": [
                    {"names": ["--flag"], "help": "Flag name.", "default": None},
                    {"names": ["--value"], "help": "New value (on/off/enum).",
                     "default": None},
                    {"names": ["--local"], "is_flag": True, "default": False,
                     "help": "Write to crosspad.local.json instead."},
                    {"names": ["--gen"], "is_flag": True, "default": False,
                     "help": "Regenerate .crosspad/build_flags.*"},
                ],
            },
            "app-profile": {
                "callback": app_profile_cmd,
                "help": "Named build recipes: list/show/apply/save.",
                "options": [
                    {"names": ["--profile-action"],
                     "help": "list | show | apply | save", "default": "list"},
                    {"names": ["--name"], "help": "Profile name.",
                     "default": None},
                    {"names": ["--remove-extra"], "is_flag": True,
                     "default": False,
                     "help": "Remove installed apps the profile omits."},
                    {"names": ["--description"], "default": "",
                     "help": "Description when saving."},
                ],
            },
            "app-sync": {
                "callback": app_sync_cmd,
                "help": "Sync manifest with existing app submodules.",
            },
            "app-manage": {
                "callback": app_manage_cmd,
                "help": "Interactive app manager (TUI).",
            },
            "board": {
                "callback": board_cmd,
                "help": "Which board revision builds and flashes target: the connected "
                        "board, else the last choice. --set remembers one.",
                "options": [
                    {"names": ["--set"], "help": "Remember this revision (v1 | v2).",
                     "default": None},
                    {"names": ["--board"], "help": "Use this revision (v1 | v2).",
                     "default": None},
                    {"names": ["--device"], "help": "crosspad-hil device id.",
                     "default": None},
                ],
            },
            "app-update": {
                "callback": app_update_cmd,
                "help": "Update installed app(s) to latest ref.",
                "options": [
                    {
                        "names": ["--app"],
                        "help": "App name to update.",
                        "default": None,
                    },
                    {
                        "names": ["--all"],
                        "is_flag": True,
                        "default": False,
                        "help": "Update all installed apps.",
                    },
                    {
                        "names": ["--force"],
                        "is_flag": True,
                        "default": False,
                        "help": "Update despite local work (backs it up first).",
                    },
                    {
                        "names": ["--dry-run"],
                        "is_flag": True,
                        "default": False,
                        "help": "Show what would happen, change nothing.",
                    },
                ],
            },
        },
    }
