import LUA_API_DOC from '../../../docs/lua-api.md?raw';

export function getAiDocs() {
    const content = (LUA_API_DOC || '').trim();
    return { content, chars: content.length, source: content ? 'embedded' : 'none', error: content ? '' : '未内置 Lua API 文档' };
}
