"""Built WebUI navigation/reconnect regression; fake machine, no hardware writes.

Run npm run build in web, then uv run --with playwright python scripts/test_navigation_browser.py.
"""
import functools
from http.server import ThreadingHTTPServer
import threading

from playwright.sync_api import sync_playwright
from test_prompt_browser import Handler, ROOT

SOCKET = """
localStorage.setItem('dashboardCompactPanels', '[]');
window.__sent = [];
window.__errors = [];
window.addEventListener('unhandledrejection', event => window.__errors.push(String(event.reason)));
class FakeSocket extends EventTarget {
  static OPEN = 1;
  constructor() {
    super(); this.readyState = 0; window.__socket = this;
  }
  send(text) { window.__sent.push(JSON.parse(text)); }
  close() { this.readyState = 3; this.dispatchEvent(new Event('close')); }
}
window.WebSocket = FakeSocket;
window.__emit = message => window.__socket.dispatchEvent(new MessageEvent('message', {data: JSON.stringify(message)}));
window.__open = () => {
  window.__socket.readyState = 1;
  window.__socket.dispatchEvent(new Event('open'));
  window.__emit({tp:'evt:status', puid:'profile-a', p:'Test espresso', m:1, ct:93});
};
window.__reply = request => window.__emit({
  tp: request.tp.replace('req:', 'res:'), rid: request.rid, profiles: [],
  profile: {id:'profile-a',label:'Test espresso',type:'standard',temperature:93,
    phases:[{name:'Brew',phase:'brew',duration:30,pump:{target:'pressure',pressure:9},targets:[]}]}
});
window.__navigate = path => {
  const link = document.createElement('a'); link.href = path;
  document.body.append(link); link.click(); link.remove();
};
"""


def check_uart_update(page):
    prior_checks = page.evaluate("window.__sent.filter(m => m.tp === 'req:ota-settings').length")
    page.evaluate("window.__emit({tp:'evt:status',hl:true,lnk:'uart'})")
    page.evaluate("window.__navigate('/settings/system')")
    page.wait_for_function("count => window.__sent.filter(m => m.tp === 'req:ota-settings').length > count", arg=prior_checks)
    page.evaluate("""window.__emit({tp:'res:ota-settings', channel:'latest', controllerOtaEnabled:false,
            displayVersion:'v1.8.36', checkError:'no_stable_release'})""")
    page.get_by_text("This fork has no Stable release.", exact=False).wait_for()
    assert page.get_by_role("button", name="Update Controller", exact=True).count() == 0
    assert page.get_by_role("button", name="Update both", exact=True).count() == 0
    page.evaluate("""window.__emit({tp:'res:ota-settings', channel:'nightly', controllerOtaEnabled:false,
            displayVersion:'v1.8.36', latestVersion:'v1.9.0-nightly.37', displayUpdateAvailable:true})""")
    update = page.get_by_role("button", name="Update Headless", exact=True)
    update.click()
    page.wait_for_function("window.__sent.some(m => m.tp === 'req:ota-start' && m.cp === 'display')")
    assert page.get_by_text("Updating Headless firmware", exact=True).count() == 0
    page.evaluate("window.__reply(window.__sent.filter(m => m.tp === 'req:ota-start').at(-1))")
    page.evaluate("window.__emit({tp:'evt:ota-progress',phase:1,progress:7})")
    page.get_by_text("7%", exact=True).wait_for()
    page.evaluate("window.__emit({tp:'evt:ota-progress',phase:6,progress:7})")
    page.get_by_text("Reconnecting to resume the firmware download", exact=True).wait_for()
    page.evaluate("window.__emit({tp:'evt:ota-progress',phase:5,progress:0,error:'image_missing'})")
    page.get_by_text("The selected release does not contain firmware for this board.", exact=True).wait_for()
    page.get_by_role("button", name="Back to updates").click()
    update.wait_for()
    assert update.is_enabled()


def check_process_controls(page):
    start = page.get_by_role('button', name='Start', exact=True)
    pause = page.get_by_role('button', name='Pause', exact=True)
    finish = page.get_by_role('button', name='Finish', exact=True)
    flush = page.get_by_role('button', name='Flush water')
    brewing = page.get_by_text('Now Brewing', exact=False)
    finished = page.get_by_text('Finished · Test espresso', exact=True)
    brew_process = dict(a=1, u=0, s='brew', e=1200, tt='time', pp=1200, pt=30000)

    def process_status(process):
        page.evaluate('process => window.__emit({tp:"evt:status", ct:93, process})', process)

    def count_commands(topic):
        return page.evaluate('tp => window.__sent.filter(msg => msg.tp === tp).length', topic)

    for manual_stop in (True, False):
        prior_starts = count_commands('req:process:activate')
        start.click()
        assert count_commands('req:process:activate') == prior_starts + 1
        process_status(brew_process)
        brewing.wait_for()
        page.evaluate('window.__emit({tp:"evt:status", p:"Test espresso"})')
        assert pause.is_visible()
        if manual_stop:
            prior_stops = count_commands('req:process:deactivate')
            pause.click()
            assert count_commands('req:process:deactivate') == prior_stops + 1
        # A brew stopped immediately may report zero elapsed milliseconds.
        process_status(dict(brew_process, a=0, e=0 if manual_stop else 30000))
        finished.wait_for()
        prior_clears = count_commands('req:process:clear')
        finish.click()
        assert count_commands('req:process:clear') == prior_clears + 1
        process_status(None)
        start.wait_for()
        finished.wait_for(state='hidden')
        brewing.wait_for(state='hidden')

    for manual_stop in (True, False):
        prior_flushes = count_commands('req:flush:start')
        flush.click()
        assert count_commands('req:flush:start') == prior_flushes + 1
        page.evaluate('window.__reply(window.__sent.filter(m => m.tp === "req:flush:start").at(-1))')
        prior_clears = count_commands('req:process:clear')
        if manual_stop:
            process_status(dict(brew_process, u=1))
            prior_stops = count_commands('req:process:deactivate')
            pause.click()
            assert count_commands('req:process:deactivate') == prior_stops + 1
            assert count_commands('req:process:clear') == prior_clears
        # The next flush can finish while the browser has only seen the start request.
        process_status(dict(brew_process, a=0, u=1))
        page.wait_for_function(
            'count => window.__sent.filter(m => m.tp === "req:process:clear").length === count + 1',
            arg=prior_clears,
        )
        if manual_stop:
            process_status(None)
        else:
            page.evaluate('window.__emit({tp:"evt:status", ct:93})')  # Older firmware leaves out process.
        start.wait_for()
        assert flush.get_attribute('aria-pressed') == 'false'
        finished.wait_for(state='hidden')
        brewing.wait_for(state='hidden')


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(ROOT / 'web/dist')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel='chrome', headless=True)
            for viewport in ({'width': 390, 'height': 844}, {'width': 1280, 'height': 900}):
                page = browser.new_page(viewport=viewport)
                errors = []
                def record_error(error):
                    errors.append(str(error))
                    print('Browser error:', error, flush=True)
                page.on('pageerror', record_error)
                page.add_init_script(SOCKET)
                page.goto(f'http://127.0.0.1:{server.server_port}/not-found')
                page.wait_for_selector('footer')
                page.evaluate("window.__navigate('/')")
                page.wait_for_selector('canvas')
                assert not page.evaluate("window.__sent.some(m => m.tp === 'req:profiles:load')")
                page.evaluate('window.__open()')
                page.wait_for_function("window.__sent.some(m => m.tp === 'req:profiles:load')")

                # Leave before the reply, return and deliver the old reply first.
                for attempt in range(10):
                    pending = page.evaluate("window.__sent.filter(m => m.tp === 'req:profiles:load').at(-1)")
                    page.evaluate('path => window.__navigate(path)', '/profiles' if attempt % 2 == 0 else '/not-found')
                    page.wait_for_function("document.querySelectorAll('canvas').length === 0")
                    page.evaluate('(request) => window.__reply(request)', pending)
                    count = page.evaluate("window.__sent.filter(m => m.tp === 'req:profiles:load').length")
                    page.evaluate("window.__navigate('/')")
                    page.wait_for_function("count => window.__sent.filter(m => m.tp === 'req:profiles:load').length > count", arg=count)

                # Reconnect with the same selected profile ID must retry its load.
                count = page.evaluate("window.__sent.filter(m => m.tp === 'req:profiles:load').length")
                page.evaluate('window.__oldSocket = window.__socket; window.__socket.close()')
                page.wait_for_function('window.__socket !== window.__oldSocket')
                page.evaluate('window.__open()')
                page.wait_for_function("count => window.__sent.filter(m => m.tp === 'req:profiles:load').length > count", arg=count)
                page.evaluate("window.__reply(window.__sent.filter(m => m.tp === 'req:profiles:load').at(-1))")
                page.wait_for_function("document.querySelectorAll('canvas').length >= 2")

                # The target number uses controller readback only on mismatch;
                # no diagnostic panel/label changes the dashboard layout.
                page.evaluate("window.__emit({tp:'evt:status',tt:85,cht:145,chc:true,ct:86,m:1})")
                target = page.get_by_label('Temperature target differs from selected target')
                target.wait_for()
                assert '145' in target.inner_text() and 'text-warning' in target.get_attribute('class')
                assert page.get_by_text('Heating details', exact=True).count() == 0
                page.evaluate("window.__emit({tp:'evt:status',tt:85,cht:85,chc:true})")
                target.wait_for(state='hidden')
                page.evaluate("window.__emit({tp:'evt:status',tt:85,cht:null,chc:false})")
                assert page.get_by_label('Temperature target differs from selected target').count() == 0

                check_process_controls(page)

                # An orientation timer queued just before leaving must not use a
                # destroyed Chart.js instance 100 ms later.
                page.evaluate("window.dispatchEvent(new Event('orientationchange')); window.__navigate('/not-found')")
                page.wait_for_function("document.querySelectorAll('canvas').length === 0")
                check_uart_update(page)
                page.wait_for_timeout(200)
                assert not errors, errors
                assert not page.evaluate('window.__errors'), page.evaluate('window.__errors')
                print(f'PASS {viewport}: navigation, reconnect, brew/flush controls and UART OTA retry/failure')
                page.close()
            browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
