"""Auto Tuning snapshot races, reconnect recovery and stop UX with a fake machine."""
import functools
import threading
from http.server import ThreadingHTTPServer
from playwright.sync_api import sync_playwright
from test_prompt_browser import Handler, ROOT
from test_navigation_browser import SOCKET

SETTINGS = dict(rlAutoTuningEnabled=True, rlProviderMode="off_board",
    rlLocalOptimizationEnabled=True, rlCPBOOptimizationRunId="run", rlCPBOBestShotId="best",
    rlBeanContextId="bean", rlGrinderContextId="grinder", rlLastRecommendationId="",
    rlBeanContexts=[dict(id="bean", name="Morning beans", status="active")],
    rlGrinderContexts=[dict(id="grinder", name="Manual grinder", status="active")])
REC = dict(tp="evt:rl:recommendation", recommendation_id="fresh", status="accepted",
    mode="cpbo_best_incumbent", next_dose_g=21.7, target_yield_g=43.4, target_ratio=2,
    grind_delta_steps_from_current=1, projected_relative_step_from_reference=3)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(ROOT / "web/dist")))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with sync_playwright() as pw:
            browser = pw.chromium.launch(channel="chrome", headless=True)
            for width, height in ((320, 568), (390, 844), (844, 390), (1280, 900)):
                page = browser.new_page(viewport=dict(width=width, height=height))
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.add_init_script(SOCKET)
                page.route("**/api/settings", lambda route: route.fulfill(json=SETTINGS))
                page.goto(f"http://127.0.0.1:{server.server_port}/not-found")
                page.wait_for_selector("footer")
                page.evaluate("window.__open()")
                held = []
                page.unroute("**/api/settings")
                page.route("**/api/settings", lambda route: held.append(route))
                page.evaluate("window.__navigate('/autotuning')")
                page.wait_for_function("window.__sent.some(m => m.tp === 'req:rl:status:refresh')")
                page.wait_for_timeout(100)
                assert held
                page.evaluate("rec => window.__emit(rec)", REC)
                for route in held:
                    route.fulfill(json=SETTINGS)
                held.clear()
                page.get_by_text("21.7g", exact=True).wait_for()
                # A later clear also wins over an older HTTP snapshot.
                page.evaluate("window.__oldSocket = window.__socket; window.__socket.close()")
                page.wait_for_function("window.__socket !== window.__oldSocket")
                page.evaluate("window.__open()")
                page.wait_for_timeout(150)
                page.evaluate("window.__emit({tp:'evt:rl:recommendation-clear'})")
                for route in held:
                    route.fulfill(json={**SETTINGS, "rlLastRecommendationId": "stale", "rlRecommendationNextDoseG": 21.7})
                held.clear()
                page.get_by_text("21.7g", exact=True).wait_for(state="hidden")
                assert page.evaluate("window.__sent.filter(m => m.tp === 'req:rl:status:refresh').length >= 2")
                page.unroute("**/api/settings")
                page.route("**/api/settings", lambda route: route.fulfill(json={**SETTINGS, "rlRestoreBestStatus": "pending"}))
                stop = page.get_by_role("button", name="Stop optimization and use best shot", exact=True)
                stop.scroll_into_view_if_needed()
                assert stop.bounding_box()["height"] >= 44
                stop.click()
                dialog = page.get_by_role("dialog")
                assert "Morning beans" in dialog.inner_text()
                assert "unanswered comparison" in dialog.inner_text()
                assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
                confirm = dialog.get_by_role("button", name="Stop and restore best", exact=True)
                confirm.click()
                page.wait_for_function("window.__sent.some(m => m.tp === 'req:rl:optimization:stop-best')")
                page.evaluate("window.__reply(window.__sent.filter(m => m.tp === 'req:rl:optimization:stop-best').at(-1))")
                dialog.wait_for(state="hidden")
                page.get_by_text("Request queued.", exact=False).wait_for()
                assert stop.is_disabled()
                page.evaluate("window.__emit({tp:'evt:rl:status',rlRestoreBestStatus:'failed'})")
                page.get_by_role("button", name="Retry restoring best shot").wait_for()
                page.evaluate("rec => window.__emit({...rec,mode:'restore_best',status:'pending'})", REC)
                assert page.get_by_role("dialog").count() == 0
                page.evaluate("window.__emit({tp:'evt:rl:status',rlRestoreBestStatus:'restored',rlCPBOStoppedByUser:true,rlOptimizationPaused:true})")
                page.get_by_text("Optimization stopped. Best dose", exact=False).wait_for()
                page.get_by_role("button", name="Resume exploration").wait_for()
                assert not errors, errors
                assert not page.evaluate("window.__errors")
                images = ROOT / ".pio/ui-review"
                images.mkdir(parents=True, exist_ok=True)
                page.screenshot(path=str(images / f"autotuning-{width}.png"), full_page=True)
                print(f"PASS {width}x{height}: snapshot races, reconnect, confirmation, pending/failure/restored states")
                page.close()
            browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
