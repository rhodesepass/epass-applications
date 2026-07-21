// 让 load_font 的 EPASS_FONTS_DIR 覆盖机制指向嵌入的字体目录
Module.preRun = Module.preRun || [];
Module.preRun.push(function() { ENV.EPASS_FONTS_DIR = '/fonts'; });
