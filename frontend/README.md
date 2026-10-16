# Pico DS5 Lua 工作台

这是 Pico DS5 的独立单页前端：全屏 Lua 编辑器、WebUSB/Web Serial 连接、设备日志、浏览器脚本库和可选 AI 助手。编辑器草稿只保存在浏览器；“加载到内存”不会写 Flash，“写入存储”才会把当前运行镜像保存到设备 KV。

## 开发

```bash
npm install
npm run dev
```

默认地址是 `http://localhost:5173`。设备 API 需要 HTTPS 或 localhost，并要求支持 WebUSB/Web Serial 的 Chromium 浏览器。

运行 `npm run build` 后，`dist/` 可直接作为静态站点发布。Vite 使用相对资源路径，支持
GitHub Pages 的仓库子路径，无需额外设置 base。

## GitHub Pages

`.github/workflows/pages.yml` 在推送到 `main` 且前端相关文件有修改时，自动执行
`npm ci`、`npm run build` 并部署 `dist/`。仅修改固件不会触发；前端目录、内置的
`docs/lua-api.md`、`scripts/default.lua`、`tools/pico-device.js` / `.mjs` 和工作流本身
的修改会触发。需要重新部署时，也可在 Actions 中手动运行 Build and Deploy Frontend。

首次使用需在仓库 Settings → Pages 中将 Source 设为 GitHub Actions。私有仓库需
GitHub 套餐支持 Pages；如果设置时提示套餐不支持，需要升级套餐或将仓库设为公开。
发布地址以工作流的 `github-pages` 部署链接为准。

## 预览生产构建

```bash
npm run preview
```

## 检查

```bash
npm run build
npm run test:ai
```
