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

                # An orientation timer queued just before leaving must not use a
                # destroyed Chart.js instance 100 ms later.
                page.evaluate("window.dispatchEvent(new Event('orientationchange')); window.__navigate('/not-found')")
                page.wait_for_function("document.querySelectorAll('canvas').length === 0")
                page.wait_for_timeout(200)
                assert not errors, errors
                assert not page.evaluate('window.__errors'), page.evaluate('window.__errors')
                print(f'PASS {viewport}: delayed startup, 10 early navigations, stale replies, reconnect and chart disposal')
                page.close()
            browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
