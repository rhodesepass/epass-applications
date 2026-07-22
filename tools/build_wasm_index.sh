#!/bin/sh
# 生成 build-wasm/ 的预览首页: 应用列表 + iframe + 共享文件系统面板。
# 文件面板直接读写 IDBFS 的 IndexedDB 库(库名=挂载点 '/data', schema 与
# emscripten library_idbfs.js 一致: 版本 21, store 'FILE_DATA', 记录
# {timestamp, mode, contents}), 所以与各 fg_ext 应用看到的是同一份数据。
# 用法: tools/build_wasm_index.sh
set -e
cd "$(dirname "$0")/.."

OUT=build-wasm/index.html

# glob 展开自带字母序; 没编出 index.html 的目录(编译失败残留)跳过。
# 显示名/类型/扩展名取自 applications/<app>/appconfig.json —— 与设备同源。
ITEMS=""
APPS_JS="["
for d in build-wasm/*/; do
    app=$(basename "$d")
    [ -f "$d/index.html" ] || continue
    name="$app"
    tag=""
    entry="{\"dir\":\"$app\",\"name\":\"$app\",\"exts\":[]}"
    cfg="applications/$app/appconfig.json"
    if [ -f "$cfg" ]; then
        name=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1],encoding='utf-8')).get('name') or sys.argv[2])" "$cfg" "$app")
        t=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1],encoding='utf-8')).get('type',''))" "$cfg")
        [ "$t" = "fg_ext" ] && tag=" · 文件启动"
        # 扩展名规范化成带点小写(ebook 的 appconfig 里混着 "md" 这种没点的)
        entry=$(python3 -c "
import json, sys
cfg = json.load(open(sys.argv[1], encoding='utf-8'))
exts = ['.' + e.lstrip('.').lower() for e in cfg.get('extensions', [])]
print(json.dumps({'dir': sys.argv[2], 'name': cfg.get('name') or sys.argv[2],
                  'exts': exts}, ensure_ascii=False))" "$cfg" "$app")
    fi
    ITEMS="$ITEMS    <li><a href=\"#$app\" data-app=\"$app\">$name<small>$app$tag</small></a></li>
"
    APPS_JS="$APPS_JS$entry,"
done
APPS_JS="${APPS_JS%,}]"

# 整文件重写, 保证幂等; JS 里 $ 多, 静态段用带引号 heredoc 免转义
cat > "$OUT" <<'EOF'
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>epass wasm 预览</title>
<style>
  html, body {
    margin: 0; height: 100%;
    background: #07131a; color: #91aeb5;
    font-family: monospace;
  }
  body { display: flex; }
  nav {
    width: 230px; flex: none;
    border-right: 1px solid #3a6874;
    padding: 16px 0;
    display: flex; flex-direction: column;
    overflow-y: auto;
  }
  nav h1 {
    font-size: 14px; font-weight: normal;
    color: #a4f06d; margin: 0 16px 12px;
  }
  nav ul { list-style: none; margin: 0; padding: 0; }
  nav a {
    display: block; padding: 6px 16px;
    color: #91aeb5; text-decoration: none;
  }
  nav a small {
    display: block; font-size: 10px; color: #5b7681;
  }
  nav a.active small, nav a:hover small { color: inherit; opacity: .7; }
  nav a:hover { color: #a4f06d; }
  nav a.active {
    color: #a4f06d; background: #3a6874;
  }
  #vfs {
    margin: 16px 12px 0; padding: 10px;
    border: 1px solid #3a6874;
    display: flex; flex-direction: column; gap: 6px; font-size: 12px;
  }
  #vfs h2 { font-size: 13px; margin: 0; color: #a4f06d; font-weight: normal; }
  #vfs .hint { font-size: 10px; color: #5b7681; margin: 0; }
  #vfs ul { display: flex; flex-direction: column; gap: 3px; }
  #vfs li { display: flex; align-items: center; gap: 4px; }
  #vfs li span { flex: 1; overflow: hidden; text-overflow: ellipsis;
                 white-space: nowrap; }
  #vfs button {
    background: #102630; color: #91aeb5; border: 1px solid #3a6874;
    font-family: monospace; font-size: 11px; padding: 1px 6px; cursor: pointer;
  }
  #vfs button:hover:enabled { background: #3a6874; color: #e8f2f2; }
  #vfs button:disabled { opacity: .4; cursor: default; }
  #vfs button.del { border-color: #5b3038; color: #b07070; padding: 1px 4px; }
  #vfs button.del:hover:enabled { background: #5b3038; color: #ffb0b0; }
  nav p.keys { font-size: 11px; margin: auto 16px 0; padding-top: 12px; }
  main { flex: 1; display: flex; }
  iframe { flex: 1; border: 0; display: none; }
  #hint { margin: auto; font-size: 13px; }
  /* 窄容器(devbox 会话页把本页塞进预览列): 上 wasm 画面 / 下列表+文件系统 */
  @media (max-width: 700px) {
    body { flex-direction: column; }
    main { order: 1; flex: 0 0 62%; min-height: 0; }
    nav {
      order: 2; flex: 1; width: auto; min-height: 0;
      border-right: 0; border-top: 1px solid #3a6874; padding-top: 10px;
    }
    nav p.keys { display: none; }
  }
</style>
</head>
<body>
<nav>
  <h1>epass wasm 预览</h1>
  <ul id="list">
EOF

printf '%s' "$ITEMS" >> "$OUT"

cat >> "$OUT" <<'EOF'
  </ul>
  <div id="vfs">
    <h2>文件系统 /data</h2>
    <p class="hint">全应用共享(IndexedDB)。"打开"= 设备上文件管理器的
      fg_ext 启动: 按扩展名匹配应用, 以文件为参数拉起。</p>
    <ul id="vfs-list"><li><span>读取中…</span></li></ul>
    <input id="vfs-upload" type="file" multiple hidden>
    <button id="vfs-add">+ 上传文件</button>
    <p class="hint" id="vfs-status"></p>
  </div>
  <p class="keys">1/&uarr;/&larr; &nbsp; 2/&darr;/&rarr;<br>3/Enter &nbsp; 4/Esc</p>
</nav>
<main>
  <p id="hint">&larr; 选一个应用, 或从文件系统打开文件</p>
  <iframe id="frame" title="app"></iframe>
</main>
<script>
EOF

printf 'var APPS = %s;\n' "$APPS_JS" >> "$OUT"

cat >> "$OUT" <<'EOF'
var frame = document.getElementById('frame');
var hint = document.getElementById('hint');
var links = document.querySelectorAll('#list a');

// 键盘事件只落在有焦点的窗口, 所以 iframe 加载完必须把焦点交进去;
// 应用启动时可能播种示例文件, 稍等一拍再刷新文件面板
frame.addEventListener('load', function () {
  frame.contentWindow.focus();
  setTimeout(vfsRefresh, 1500);
});

function showFrame(src) {
  hint.style.display = 'none';
  frame.style.display = 'block';
  if (frame.getAttribute('src') !== src) frame.src = src;
  else frame.contentWindow.focus();
}

function apply() {
  var app = location.hash.slice(1);
  var found = false;
  links.forEach(function (a) {
    var on = a.dataset.app === app;
    a.classList.toggle('active', on);
    if (on) found = true;
  });
  if (!found) return;
  showFrame(app + '/index.html');
}

window.addEventListener('hashchange', apply);
apply();

/* ---- 共享文件面板: 直接操作 IDBFS 的 IndexedDB 库 ---- */
var VFS_DB = '/data', VFS_VER = 21, VFS_STORE = 'FILE_DATA';
var S_IFMT = 0xF000, S_IFREG = 0x8000, REG_MODE = 0x81B6; /* 0100666 */

function vfsStatus(t) { document.getElementById('vfs-status').textContent = t; }

function vfsDb(cb) {
  var req = indexedDB.open(VFS_DB, VFS_VER);
  req.onupgradeneeded = function (e) {
    /* 库还没被任何应用建过: 按 library_idbfs.js 的样式建 */
    var db = e.target.result;
    var store = db.objectStoreNames.contains(VFS_STORE)
      ? e.target.transaction.objectStore(VFS_STORE)
      : db.createObjectStore(VFS_STORE);
    if (!store.indexNames.contains('timestamp'))
      store.createIndex('timestamp', 'timestamp', { unique: false });
  };
  req.onsuccess = function (e) { cb(e.target.result); };
  req.onerror = function () { vfsStatus('IndexedDB 打开失败'); };
}

function vfsList(cb) {
  vfsDb(function (db) {
    var out = [];
    var cur = db.transaction(VFS_STORE, 'readonly')
                .objectStore(VFS_STORE).openCursor();
    cur.onsuccess = function (e) {
      var c = e.target.result;
      if (!c) { db.close(); cb(out); return; }
      var path = c.primaryKey, v = c.value;
      var name = path.slice(VFS_DB.length + 1);
      if (name && name.indexOf('/') < 0 && name[0] !== '.' &&
          v && ((v.mode & S_IFMT) === S_IFREG))
        out.push(name);
      c.continue();
    };
  });
}

function vfsMatchApps(name) {
  var dot = name.lastIndexOf('.');
  var ext = dot >= 0 ? name.slice(dot).toLowerCase() : '';
  return APPS.filter(function (a) { return a.exts.indexOf(ext) >= 0; });
}

function vfsRefresh() {
  vfsList(function (names) {
    var list = document.getElementById('vfs-list');
    list.innerHTML = '';
    if (!names.length) {
      list.innerHTML = '<li><span>(空 — 先上传文件)</span></li>';
      return;
    }
    names.sort().forEach(function (name) {
      var li = document.createElement('li');
      var span = document.createElement('span');
      span.textContent = name;
      span.title = name;
      li.appendChild(span);
      var apps = vfsMatchApps(name);
      if (apps.length) {
        apps.forEach(function (a) {
          var b = document.createElement('button');
          b.textContent = '打开';
          b.title = '用' + a.name + '打开';
          b.onclick = function () {
            /* 文件启动不属于任何列表项, 清掉高亮 */
            if (location.hash) history.replaceState(null, '', ' ');
            links.forEach(function (l) { l.classList.remove('active'); });
            showFrame(a.dir + '/index.html?open=' +
                      encodeURIComponent('/data/' + name));
          };
          li.appendChild(b);
        });
      } else {
        var nb = document.createElement('button');
        nb.textContent = '无关联';
        nb.disabled = true;
        li.appendChild(nb);
      }
      var del = document.createElement('button');
      del.textContent = '✕';
      del.className = 'del';
      del.title = '删除';
      del.onclick = function () {
        vfsDb(function (db) {
          var tx = db.transaction(VFS_STORE, 'readwrite');
          tx.objectStore(VFS_STORE).delete(VFS_DB + '/' + name);
          tx.oncomplete = function () {
            db.close();
            vfsStatus('已删除: ' + name);
            vfsRefresh();
          };
        });
      };
      li.appendChild(del);
      list.appendChild(li);
    });
  });
}

document.getElementById('vfs-add').onclick = function () {
  document.getElementById('vfs-upload').click();
};
document.getElementById('vfs-upload').onchange = function () {
  var input = this;
  var files = Array.from(input.files);
  Promise.all(files.map(function (f) { return f.arrayBuffer(); }))
    .then(function (bufs) {
      vfsDb(function (db) {
        var tx = db.transaction(VFS_STORE, 'readwrite');
        var store = tx.objectStore(VFS_STORE);
        files.forEach(function (f, i) {
          store.put({ timestamp: new Date(), mode: REG_MODE,
                      contents: new Uint8Array(bufs[i]) },
                    VFS_DB + '/' + f.name);
        });
        tx.oncomplete = function () {
          db.close();
          vfsStatus('已上传 ' + files.length + ' 个文件');
          vfsRefresh();
          input.value = '';
        };
      });
    });
};

vfsRefresh();
</script>
</body>
</html>
EOF

echo "==> $OUT"
