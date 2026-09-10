"""Exercise the built app in Chrome with a fake machine WebSocket.

Run npm run build in web first, then uv run --with playwright python
scripts/test_prompt_browser.py. Uses installed Chrome; no machine or cloud.
"""
import functools
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path.startswith('/api/'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"rlAutoTuningEnabled": true}')
            return
        if not self.path.startswith('/assets/'):
            self.path = '/index.html'
        super().do_GET()


FAKE_SOCKET = """
window.__sent = [];
class FakeSocket extends EventTarget {
  static OPEN = 1;
  constructor() {
    super(); this.readyState = 1; window.__socket = this;
    setTimeout(() => this.dispatchEvent(new Event('open')), 10);
  }
  send(text) { window.__sent.push(JSON.parse(text)); }
  close() { this.readyState = 3; }
}
window.WebSocket = FakeSocket;
window.__emit = message => window.__socket.dispatchEvent(new MessageEvent('message', {data: JSON.stringify(message)}));
"""

PROMPT = dict(tp='evt:rl:shot-complete', preference_feedback_required=True,
              shot_id='new', anchor_shot_id='anchor', install_id='install', optimization_run_id='run',
              prompt_revision=1, comparison_mode='best_incumbent',
              taste_goal={'mode': 'custom', 'targets': {'sweet': 'high'}},
              taste_goal_summary='Sweet: high', anchor=dict(timestamp=1720000000,
                  relative_grind_steps_from_reference=5, current_absolute_step=15,
                  dose_g=18, target_yield_g=36, beverage_out_g=35.5, profile_label='Morning espresso'))


def overlap(a, b):
    return a['x'] < b['x'] + b['width'] and b['x'] < a['x'] + a['width'] and a['y'] < b['y'] + b['height'] and b['y'] < a['y'] + a['height']


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(ROOT / 'web/dist')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    images = ROOT / '.pio/ui-review'
    images.mkdir(parents=True, exist_ok=True)
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel='chrome', headless=True)
            for width, height in ((320, 568), (390, 844), (844, 390), (1280, 900)):
                page = browser.new_page(viewport={'width': width, 'height': height})
                page.on('pageerror', lambda error: print('Browser error:', error.stack, flush=True))
                page.on('console', lambda message: print('Browser console:', message.text, flush=True) if message.type == 'error' else None)
                page.add_init_script(FAKE_SOCKET)
                page.goto(f'http://127.0.0.1:{server.server_port}/not-found')
                page.wait_for_selector('footer')
                page.evaluate('(prompt) => window.__emit(prompt)', PROMPT)
                dialog = page.get_by_role('dialog')
                dialog.wait_for()
                assert 'Sweet: high' in dialog.inner_text()
                assert 'Morning espresso' in dialog.inner_text()
                page.screenshot(path=str(images / f'prompt-{width}.png'))
                page.get_by_role('button', name='Minimize shot prompt').click()
                button = page.get_by_role('button', name='Open shot prompts (1 pending)')
                button.wait_for()
                box = button.bounding_box()
                assert box['width'] >= 44 and box['height'] >= 44
                assert box['y'] >= 0 and box['y'] + box['height'] <= height
                menu = page.get_by_role('button', name='Open menu', exact=True)
                if menu.is_visible():
                    assert not overlap(box, menu.bounding_box())
                    menu.click()
                    page.get_by_role('button', name='Close menu', exact=True).click()
                assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth')
                page.screenshot(path=str(images / f'minimized-{width}.png'))
                button.click()
                page.get_by_role('button', name="Can't compare / don't remember").click()
                assert page.evaluate('window.__sent.at(-1).label') == 'abstain'
                assert dialog.is_visible()  # Sending alone does not resolve it.
                page.evaluate("window.__emit({tp:'evt:rl:preference-resolved',shot_id:'new',optimization_run_id:'run',prompt_revision:1})")
                dialog.wait_for(state='hidden')
                assert page.get_by_role('button', name='Open shot prompts (1 pending)').count() == 0
                # A disconnected socket must retain the answer UI with a clear error.
                page.evaluate('(prompt) => window.__emit(prompt)', {**PROMPT, 'shot_id': 'next', 'prompt_revision': 2})
                page.evaluate('window.__socket.readyState = 3')
                page.get_by_role('button', name="Can't compare / don't remember").click()
                assert 'Not connected' in page.get_by_role('alert').inner_text()
                page.evaluate("window.__emit({tp:'evt:rl:prompts-clear'})")
                page.evaluate('window.__socket.readyState = 1')
                recipe = dict(tp='evt:rl:dose-confirmation', shot_id='recipe', prompt_revision=3,
                              dose_target_g=18, grind_setting=12.5, grind_is_absolute=True, dose_measured=False)
                page.evaluate('(prompt) => window.__emit(prompt)', recipe)
                dialog = page.get_by_role('dialog')
                assert '12.5' in dialog.inner_text() and '18.0g' in dialog.inner_text()
                page.get_by_role('button', name='Change values', exact=True).click()
                page.get_by_label('Actual grinder setting').fill('13.2')
                page.get_by_label('Actual dose in grams').fill('0')
                before = page.evaluate('window.__sent.length')
                page.get_by_role('button', name='Save recipe', exact=True).click()
                assert 'Enter a valid' in page.get_by_role('alert').inner_text()
                assert page.evaluate('window.__sent.length') == before
                page.get_by_label('Actual dose in grams').fill('19')
                page.screenshot(path=str(images / f'recipe-edit-{width}.png'))
                page.get_by_role('button', name='Save recipe', exact=True).click()
                sent = page.evaluate('window.__sent.at(-1)')
                assert sent['action'] == 'change' and sent['grind_setting'] == 13.2 and sent['dose_g'] == 19
                assert dialog.is_visible()
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'recipe',prompt_revision:2,persisted:true})")
                assert dialog.is_visible()  # A stale receipt cannot resolve this answer.
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'recipe',prompt_revision:3,persisted:false})")
                assert dialog.is_visible()
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'recipe',prompt_revision:3,persisted:true})")
                page.get_by_role('heading', name='Recipe saved', exact=True).wait_for()
                assert 'Waiting for EspressoRL' in dialog.inner_text()
                page.evaluate("window.__emit({tp:'evt:rl:shot-complete',shot_id:'other',preference_feedback_required:false})")
                assert 'Waiting for EspressoRL' in dialog.inner_text()
                page.evaluate("window.__emit({tp:'evt:rl:shot-complete',shot_id:'recipe',preference_feedback_required:false})")
                page.get_by_role('heading', name='Shot received by EspressoRL', exact=True).wait_for()
                assert page.get_by_role('link', name='View Auto-Tuning status').get_attribute('href') == '/autotuning'
                page.get_by_role('button', name='Minimize shot prompt').click()
                page.get_by_role('button', name='Open shot prompts (1 pending)').click()
                assert 'Shot received by EspressoRL' in dialog.inner_text()
                page.evaluate('(prompt) => window.__emit(prompt)', {**PROMPT, 'shot_id':'recipe', 'prompt_revision':4})
                assert 'Which is closer to your goal?' in dialog.inner_text()
                page.evaluate("window.__emit({tp:'evt:rl:prompts-clear'})")
                page.evaluate('(prompt) => window.__emit(prompt)', {**recipe, 'shot_id':'unknown', 'prompt_revision':4})
                page.get_by_role('button', name='Not sure', exact=True).click()
                assert page.evaluate('window.__sent.at(-1).action') == 'unknown'
                assert dialog.is_visible()
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'unknown',prompt_revision:4,persisted:true,followed:false})")
                page.get_by_role('heading', name='Recipe saved', exact=True).wait_for()
                assert 'marked uncertain' in dialog.inner_text()
                page.evaluate("window.__emit({tp:'evt:rl:prompts-clear'})")
                page.evaluate('(prompt) => window.__emit(prompt)', {**recipe, 'shot_id':'confirmed', 'prompt_revision':5, 'dose_measured':True})
                assert 'Dose was measured' in dialog.inner_text()
                page.evaluate('window.__socket.readyState = 3')
                page.get_by_role('button', name='Yes', exact=True).click()
                assert 'Not connected' in page.get_by_role('alert').inner_text()
                assert dialog.is_visible()
                page.evaluate('window.__socket.readyState = 1')
                page.get_by_role('button', name='Yes', exact=True).click()
                assert page.evaluate('window.__sent.at(-1).action') == 'confirm'
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'confirmed',prompt_revision:5,persisted:true,followed:true})")
                page.get_by_role('heading', name='Recipe saved', exact=True).wait_for()
                page.evaluate("window.__emit({tp:'evt:rl:recommendation',recommendation_id:'next-recipe',status:'pending'})")
                assert 'Recipe saved' not in dialog.inner_text()
                # A fast recommendation arriving before the saved receipt must
                # not be hidden behind a new waiting screen.
                page.evaluate("window.__emit({tp:'evt:rl:prompts-clear'})")
                page.evaluate('(prompt) => window.__emit(prompt)', {**recipe, 'shot_id':'fast', 'prompt_revision':6})
                page.get_by_role('button', name='Yes', exact=True).click()
                page.evaluate("window.__emit({tp:'evt:rl:recommendation',recommendation_id:'fast-recipe',status:'pending'})")
                page.evaluate("window.__emit({tp:'evt:rl:dose-confirmation-resolved',shot_id:'fast',prompt_revision:6,persisted:true,followed:true})")
                page.get_by_role('heading', name='Confirm the recipe you used', exact=True).wait_for(state='hidden')
                assert 'Recipe saved' not in dialog.inner_text()
                page.close()
                print(f'PASS prompts: {width}x{height}, dock/menu separation, edited recipe, unknown/confirmed outcomes and durable replies')
            browser.close()
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
