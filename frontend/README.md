# Pico DS5 Lua 工作台

这是 Pico DS5 的独立单页前端：全屏 Lua 编辑器、WebUSB/Web Serial 连接、设备日志、浏览器脚本库和可选 AI 助手。编辑器草稿只保存在浏览器；“加载到内存”不会写 Flash，“写入存储”才会把当前运行镜像保存到设备 KV。

## 开发

```bash
npm install
npm run dev
```

默认地址是 `http://localhost:5173`。设备 API 需要 HTTPS 或 localhost，并要求支持 WebUSB/Web Serial 的 Chromium 浏览器。

这个单文件可以直接作为静态页面发布。

## 预览生产构建

```bash
npm run preview
```

## 检查

```bash
npm run build
npm run test:ai
```
