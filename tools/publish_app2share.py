#!/usr/bin/env python3
"""把 applications/<app> 打成 app2share 商店包并生成 manifest.json。

规约见 ../devman_web/public/app2share/README.md:
  apps/<name>_v<app_ver>.zip + previews/ + manifest.json

用法:
  tools/publish_app2share.py [--build] [--out DIR] [--changelog APP=文本] [app ...]

默认处理全部带 appconfig.json 的应用;产物写到 --out
(默认 ../devman_web/public/app2share)。zip 内容来自 dist/<app>/,
appconfig.json 始终用源码侧(保证 app_ver/uuid 与商店一致)。
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APPS_DIR = ROOT / "applications"
DIST_DIR = ROOT / "dist"
DEFAULT_OUT = ROOT.parent / "devman_web" / "public" / "app2share"

TYPE_BADGE = {"fg": "前台", "bg": "后台", "fg_ext": "扩展启动"}


def list_apps(names: list[str] | None) -> list[str]:
    if names:
        return names
    apps = []
    for p in sorted(APPS_DIR.iterdir()):
        if p.is_dir() and (p / "appconfig.json").is_file():
            apps.append(p.name)
    return apps


def load_config(app: str) -> dict:
    path = APPS_DIR / app / "appconfig.json"
    if not path.is_file():
        raise FileNotFoundError(f"没有 {path}")
    cfg = json.loads(path.read_text(encoding="utf-8"))
    for key in ("uuid", "name", "executable"):
        if key not in cfg:
            raise ValueError(f"{app}: appconfig.json 缺 {key}")
    if not isinstance(cfg.get("executable"), dict) or "file" not in cfg["executable"]:
        raise ValueError(f"{app}: executable.file 无效")
    app_ver = cfg.get("app_ver", 0)
    if not isinstance(app_ver, int) or app_ver < 0:
        raise ValueError(f"{app}: app_ver 须为非负整数, 实际 {app_ver!r}")
    return cfg


def uuid_prefix(uuid: str) -> str:
    return uuid.replace("-", "")[:8]


def build_badges(cfg: dict) -> list[str]:
    badges: list[str] = []
    app_type = cfg.get("type")
    if app_type in TYPE_BADGE:
        badges.append(TYPE_BADGE[app_type])
    exts = cfg.get("extensions")
    if isinstance(exts, list) and exts:
        badges.append(" ".join(str(e) for e in exts))
    screens = cfg.get("screens")
    if isinstance(screens, list) and screens:
        badges.append(str(screens[0]))
    return badges


def ensure_built(app: str) -> None:
    script = ROOT / "tools" / "package_app.sh"
    print(f"  build {app} …")
    subprocess.run([str(script), app], cwd=ROOT, check=True)


def make_zip(app: str, cfg: dict, zip_path: Path) -> None:
    dist = DIST_DIR / app
    if not dist.is_dir():
        raise FileNotFoundError(
            f"没有 dist/{app}/, 先跑 tools/package_app.sh {app} 或加 --build"
        )
    exe = cfg["executable"]["file"]
    if not (dist / exe).is_file():
        raise FileNotFoundError(f"dist/{app}/{exe} 不存在")

    zip_path.parent.mkdir(parents=True, exist_ok=True)
    if zip_path.exists():
        zip_path.unlink()

    src_cfg = (APPS_DIR / app / "appconfig.json").read_bytes()

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(dist.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(dist).as_posix()
            # 源码侧 appconfig 为准(dist 可能是旧包,缺 app_ver)
            if rel == "appconfig.json":
                continue
            zf.write(path, f"{app}/{rel}")
        zf.writestr(f"{app}/appconfig.json", src_cfg)


def process_app(
    app: str,
    out: Path,
    *,
    do_build: bool,
    changelog: str | None,
    old_entries: dict[str, dict],
) -> dict:
    cfg = load_config(app)
    if do_build:
        ensure_built(app)

    app_ver = int(cfg.get("app_ver", 0))
    uuid = cfg["uuid"]
    prefix = uuid_prefix(uuid)

    zip_name = f"{app}_v{app_ver}.zip"
    zip_rel = f"apps/{zip_name}"
    zip_path = out / zip_rel
    make_zip(app, cfg, zip_path)

    icon_src = APPS_DIR / app / "icon.png"
    if not icon_src.is_file():
        # 回退到 dist 里的
        icon_src = DIST_DIR / app / "icon.png"
    if not icon_src.is_file():
        raise FileNotFoundError(f"{app}: 没有 icon.png")

    icon_rel = f"previews/{prefix}_icon.png"
    (out / "previews").mkdir(parents=True, exist_ok=True)
    shutil.copy2(icon_src, out / icon_rel)

    entry = {
        "uuid": uuid,
        "name": cfg.get("name", app),
        "desc": cfg.get("description", ""),
        "app_ver": app_ver,
        "ver_name": f"{app_ver}.0.0",
        "app_type": cfg.get("type") if cfg.get("type") in TYPE_BADGE else None,
        "icon": icon_rel,
        "download_url": zip_rel,
        "badges": build_badges(cfg),
        "zip": f"{app}.zip",
    }
    if entry["app_type"] is None:
        del entry["app_type"]

    # changelog: 命令行 > 旧 manifest 保留 > 省略
    cl = changelog
    if cl is None and uuid in old_entries:
        cl = old_entries[uuid].get("changelog")
    if cl:
        entry["changelog"] = cl

    return entry


def load_old_manifest(out: Path) -> dict[str, dict]:
    path = out / "manifest.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}
    if not isinstance(data, list):
        return {}
    return {
        e["uuid"]: e
        for e in data
        if isinstance(e, dict) and isinstance(e.get("uuid"), str)
    }


def parse_changelogs(items: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise SystemExit(f"--changelog 格式应为 APP=文本, 得到: {item!r}")
        app, text = item.split("=", 1)
        result[app.strip()] = text.strip()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="生成 app2share 商店清单与 zip 包")
    parser.add_argument(
        "apps",
        nargs="*",
        help="应用目录名(默认全部)",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="先调用 package_app.sh 交叉编译再打包",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"输出目录(默认 {DEFAULT_OUT})",
    )
    parser.add_argument(
        "--changelog",
        action="append",
        default=[],
        metavar="APP=文本",
        help="指定某应用的 changelog(可多次)",
    )
    parser.add_argument(
        "--merge",
        action="store_true",
        help="只更新给定应用,保留 manifest 里其他条目",
    )
    args = parser.parse_args()

    out: Path = args.out.resolve()
    (out / "apps").mkdir(parents=True, exist_ok=True)
    (out / "previews").mkdir(parents=True, exist_ok=True)

    apps = list_apps(args.apps or None)
    if not apps:
        print("没有可处理的应用", file=sys.stderr)
        return 1

    changelogs = parse_changelogs(args.changelog)
    old = load_old_manifest(out)

    entries: list[dict] = []
    errors = 0
    for app in apps:
        print(f"[{app}]")
        try:
            entry = process_app(
                app,
                out,
                do_build=args.build,
                changelog=changelogs.get(app),
                old_entries=old,
            )
            entries.append(entry)
            print(f"  → {entry['download_url']}  (app_ver={entry['app_ver']})")
        except Exception as e:
            errors += 1
            print(f"  ERROR: {e}", file=sys.stderr)

    if args.merge and old:
        # 本次处理的覆盖同 uuid;其余旧条目保留
        done = {e["uuid"] for e in entries}
        for e in old.values():
            if e["uuid"] not in done:
                entries.append(e)

    entries.sort(key=lambda e: e["name"])
    manifest_path = out / "manifest.json"
    manifest_path.write_text(
        json.dumps(entries, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"\nDone: {len(entries)} apps → {manifest_path}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
