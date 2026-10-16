// Read the board initially; keep explicit local edits across reconnects.
export class WorkspaceModel {
    constructor(session) {
        this.session = session;
        this.listeners = new Set();
        this.snapshot = { script: '', revision: 0, ready: false, hasEdits: false, retained: false, notice: '', noticeSeq: 0 };
        this.unsubscribe = session.subscribe(() => {
            if (session.getSnapshot().state !== 'connected') {
                this.update({ ready: false, retained: this.snapshot.retained || this.snapshot.ready,
                    revision: this.snapshot.revision + 1, notice: '' });
            }
        });
    }
    dispose = () => this.unsubscribe();
    assertReady(epoch = this.session.epoch) {
        this.session.assertSession(epoch);
        if (!this.snapshot.ready) throw new Error('请先读取设备脚本');
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
    edit = (script, notice = '', epoch = this.session.epoch) => {
        this.assertReady(epoch);
        this.update({ script, hasEdits: script !== this.baseline, revision: this.snapshot.revision + 1, notice });
    };
    notify = notice => this.update({ notice });
    async read({ automatic = false } = {}) {
        const epoch = this.session.epoch;
        const revision = this.snapshot.revision;
        const preserve = automatic && this.snapshot.hasEdits;
        const source = await this.session.read();
        this.session.assertSession(epoch);
        if (!preserve && revision === this.snapshot.revision) {
            this.baseline = source;
            this.update({ script: source, ready: true, retained: false, hasEdits: false, revision: revision + 1,
                notice: '已读取设备当前运行脚本。' });
        } else this.update({ ready: true, retained: false, notice: source === this.getScript()
            ? '已重连，保留的编辑内容与设备运行脚本一致。'
            : '已读取设备状态并保留编辑内容；与设备脚本不同，可点击“读取设备”替换。' });
        return source;
    }
    async run(source = this.getScript(), options) {
        this.assertReady();
        const epoch = this.session.epoch;
        await this.session.run(source, options);
        this.session.assertSession(epoch);
        this.notify(source === this.getScript() ? '已加载到内存。验证正常后点击“写入存储”。' : '上传的版本已运行；编辑器有后续改动，尚未加载。');
    }
    async save() {
        this.assertReady();
        const epoch = this.session.epoch;
        const source = this.getScript();
        await this.session.save(source);
        this.session.assertSession(epoch);
        this.baseline = source;
        this.update({ hasEdits: source !== this.getScript() });
        this.notify(source === this.getScript() ? '当前运行脚本已写入 KV，断电后会自动加载。' : '运行版本已写入 KV；编辑器的后续改动尚未加载。');
    }
    async switchRunning(kind) {
        this.assertReady();
        const epoch = this.session.epoch;
        const revision = this.snapshot.revision;
        const source = kind === 'default' ? await this.session.restoreDefault() : await this.session.reload();
        this.session.assertSession(epoch);
        if (revision === this.snapshot.revision) {
            this.baseline = source;
            this.edit(source, kind === 'default' ? '已运行预制脚本，KV 保持原样。' : '已从 KV 加载运行。');
        }
        else this.notify('设备运行脚本已切换，保留操作期间产生的编辑器改动。');
    }
}
