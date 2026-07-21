// fg_ext 应用不在页面加载时自启: 由 VFS 面板选好文件后 callMain([路径]) 启动,
// 模拟设备上"文件管理器按扩展名打开文件"的流程
Module.noInitialRun = true;
Module.preRun = Module.preRun || [];
Module.preRun.push(function() {
  ENV.EPASS_FONTS_DIR = '/fonts';
  // /data(上传的文件)与 /ebook_state(阅读进度)挂 IDBFS, 跨刷新持久;
  // syncfs(true) 是异步的, 用 run dependency 拖住 runtime 直到导入完成
  addRunDependency('idbfs-mount');
  FS.mkdir('/data');
  FS.mount(IDBFS, {}, '/data');
  FS.mkdir('/ebook_state');
  FS.mount(IDBFS, {}, '/ebook_state');
  FS.syncfs(true, function() { removeRunDependency('idbfs-mount'); });
});
