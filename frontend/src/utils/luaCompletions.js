import { completeFromList } from '@codemirror/autocomplete';

const entries = `and break do else elseif end false for function goto if in local nil not or repeat return then true until while
type tonumber tostring pairs ipairs error assert pcall select next setmetatable getmetatable rawget rawset
on_key on_mouse_btn on_mouse_move on_mouse_wheel on_gamepad_btn on_gamepad_abs on_motion on_touchpad on_boot_button on_dropped tick
dev_enable kbd_key kbd_release_all kbd_is_down kbd_is_on kbd_led kbd_state mouse_move mouse_btn mouse_release_all mouse_is_down mouse_is_on mouse_state io_is_on pad_btn pad_abs pad_touch pad_motion pad_disconnect print warn
string.format string.match string.find string.sub string.gsub string.rep string.len string.byte string.char string.lower string.upper
table.insert table.remove table.concat table.sort table.unpack math.floor math.ceil math.abs math.min math.max math.random math.sqrt math.pi coroutine.create coroutine.resume coroutine.yield coroutine.status coroutine.wrap`.split(/\s+/);
const options = entries.map(label => ({ label, type: /^(and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|repeat|return|then|true|until|while)$/.test(label) ? 'keyword' : 'function' }));

export function luaCompletions(context) {
    const word = context.matchBefore(/[\w.]*/);
    if (!word || (word.from === word.to && !context.explicit)) return null;
    return completeFromList(options)(context);
}
