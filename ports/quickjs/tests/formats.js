{
    check(new Date(0).toISOString() === '1970-01-01T00:00:00.000Z', 'Date ISO fields');
    check(new Date(-1).toISOString() === '1969-12-31T23:59:59.999Z', 'Date negative epoch');
    check(new Date('+040000-02-03T04:05:06.007Z').toISOString() === '+040000-02-03T04:05:06.007Z',
          'Date extended year above signed C16 int');
    check(new Date('-000001-02-03T04:05:06.007Z').toISOString() === '-000001-02-03T04:05:06.007Z',
          'Date negative extended year');
    check(new Date(0).toUTCString() === 'Thu, 01 Jan 1970 00:00:00 GMT', 'Date UTC format and dynamic width');
    check(JSON.stringify('\u0001\ud800') === '"\\u0001\\ud800"', 'JSON code unit hex escapes');
    check(RegExp.escape('a-\u2028') === '\\x61\\x2d\\u2028', 'RegExp hexadecimal escape formatting');
    let message = '';
    try { JSON.parse('{"x": @}'); } catch (e) { message = e.message; }
    check(message.includes('@'), 'JSON unexpected token precision');
    try { Function('return ('); } catch (e) { message = e.message; }
    check(message.includes("')'") || message.includes('unexpected') || message.includes('expecting'), 'parser character diagnostic');
    let stack = '';
    try { Function('\n'.repeat(40000) + 'throw new Error("line probe")')(); }
    catch(e) { stack = e.stack; }
    check(stack.includes(':40003:'), 'stack trace line exceeds signed C16 int');
    check(Math.abs(Math.acosh(2) - 1.3169578969248166) < 1e-14 &&
          Math.abs(Math.sin(0.5) - 0.479425538604203) < 1e-14, 'accelerated Math through guest functions');
}
