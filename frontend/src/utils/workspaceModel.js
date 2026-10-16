export const DRAFT_KEY = 'ds5_lua_draft_v1';

// Editor state is independent of the board. Late reads and reconnects must not
// replace a draft edited while an async device operation was in flight.
export class WorkspaceModel {
    constructor(session, defaultSource, storage = globalThis.localStorage) {
        this.session = session;
        this.storage = storage;
        this.listeners = new Set();
        let draft = null;
        try {
            const value = JSON.parse(storage?.getItem(DRAFT_KEY) || 'null');
            if (value && typeof value.script === 'string') draft = value;
        } catch { /* unavailable storage or a damaged draft: start with the template */ }
        this.snapshot = { script: draft?.script ?? defaultSource, revision: 0, hasDraft: !!draft,
            draftError: '', notice: draft ? '已恢复浏览器草稿，连接设备不会覆盖它。' : '', noticeSeq: 0 };
    }
    getSnapshot = () => this.snapshot;
    getScript = () => this.snapshot.script;
    subscribe = fn => { this.listeners.add(fn); return () => this.listeners.delete(fn); };
    update(patch) {
        const next = { ...this.snapshot, ...patch };
        // noticeSeq：提示文本可能重复（同一操作连做两次），UI 靠这个自增序号判断
        // "又来了条新提示"，从而重置自动消失计时。
        if ('notice' in patch) next.noticeSeq = (this.snapshot.noticeSeq || 0) + 1;
        this.snapshot = next;
        for (const fn of this.listeners) fn();
    }
    edit = (script, notice = '') => {
        let draftError = '';
        try {
            if (!this.storage) throw new Error('storage unavailable');
            this.storage.setItem(DRAFT_KEY, JSON.stringify({ script, timestamp: Date.now() }));
        } catch { draftError = '浏览器草稿保存失败，请导出文件以保留编辑内容。'; }
        this.update({ script, revision: this.snapshot.revision + 1, hasDraft: true, draftError, notice });
    };
    notify = notice => this.update({ notice });
    async read({ automatic = false } = {}) {
        const revision = this.snapshot.revision;
        const preserve = automatic && this.snapshot.hasDraft;
        const source = await this.session.read();
        if (revision !== this.snapshot.revision || preserve) {
            this.notify(source === this.getScript() ? '草稿与设备运行脚本一致。' : '已连接。保留编辑器草稿；可点击“读取设备”载入当前运行脚本。');
        } else this.edit(source, '已读取当前运行脚本，尚未修改设备存储。');
        return source;
    }
    async run(source = this.getScript(), options) {
        await this.session.run(source, options);
        this.notify(source === this.getScript() ? '已加载到内存。验证正常后点击“写入存储”。' : '上传的版本已运行；编辑器有后续改动，尚未加载。');
    }
    async save() {
        const source = this.getScript();
        await this.session.save(source);
        this.notify(source === this.getScript() ? '当前运行脚本已写入 KV，断电后会自动加载。' : '运行版本已写入 KV；编辑器的后续改动尚未加载。');
    }
    async switchRunning(kind) {
        const revision = this.snapshot.revision;
        const source = kind === 'default' ? await this.session.restoreDefault() : await this.session.reload();
        if (revision === this.snapshot.revision) this.edit(source, kind === 'default' ? '已运行预制脚本，KV 保持原样。' : '已从 KV 加载运行。');
        else this.notify('设备运行脚本已切换，保留操作期间产生的编辑器改动。');
    }
}
