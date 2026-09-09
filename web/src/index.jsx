import './style.css';
import { initializeTheme } from './utils/themeManager.js';
import { hardwareScaleDisabled } from './config/features.js';

if (import.meta.env.DEV) {
  // Dev-only Preact warnings; stripped from production builds.
  import('preact/debug');
}

import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { LocationProvider, Router, Route, ErrorBoundary, useLocation } from 'preact-iso';
import lazy from 'preact-iso/lazy';

import { AutoTuningPromptOverlay } from './components/AutoTuningPromptOverlay.jsx';
import ApiService, { ApiServiceContext } from './services/ApiService.js';
import { Navigation } from './components/Navigation.jsx';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faBars } from '@fortawesome/free-solid-svg-icons/faBars';

// Each page lazy-loads as its own Vite chunk so the initial bundle stays small.
// Chart.js, FontAwesome icon sets, and the analyzer/statistics views are too
// large to ship up-front on the ESP32's slow WiFi pipe.
const Home = lazy(() => import('./pages/Home/index.jsx').then(m => m.Home));
const NotFound = lazy(() => import('./pages/_404.jsx').then(m => m.NotFound));
const Settings = lazy(() => import('./pages/Settings/index.jsx').then(m => m.Settings));
const ProfileList = lazy(() => import('./pages/ProfileList/index.jsx').then(m => m.ProfileList));
const ProfileEdit = lazy(() => import('./pages/ProfileEdit/index.jsx').then(m => m.ProfileEdit));
const ShotHistory = lazy(() => import('./pages/ShotHistory/index.jsx').then(m => m.ShotHistory));
const ShotAnalyzer = lazy(() => import('./pages/ShotAnalyzer/index.jsx').then(m => m.ShotAnalyzer));
const StatisticsPage = lazy(() =>
  import('./pages/Statistics/index.jsx').then(m => m.StatisticsPage),
);
const DashboardSettings = lazy(() =>
  import('./pages/DashboardSettings/index.jsx').then(m => m.DashboardSettings),
);
const AutoTuning = lazy(() => import('./pages/AutoTuning/index.jsx').then(m => m.AutoTuning));
const ScaleCalibration = hardwareScaleDisabled
  ? null
  : lazy(() => import('./pages/ScaleCalibration/index.jsx').then(m => m.ScaleCalibration));

const apiService = new ApiService();
const DESKTOP_NAV_COLLAPSED_STORAGE_KEY = 'gaggimate.desktopNavCollapsed';

function readInitialDesktopNavCollapsed() {
  try {
    return window.localStorage.getItem(DESKTOP_NAV_COLLAPSED_STORAGE_KEY) !== 'false';
  } catch {
    return true;
  }
}

const RedirectTo = to =>
  function Redirect() {
    const loc = useLocation();
    useEffect(() => {
      loc.route(to, true);
    }, [loc]);
    return null;
  };

export function App() {
  const [navCollapsed, setNavCollapsed] = useState(readInitialDesktopNavCollapsed);

  useEffect(() => {
    try {
      window.localStorage.setItem(DESKTOP_NAV_COLLAPSED_STORAGE_KEY, String(navCollapsed));
    } catch {
      // Ignore storage write failures so the navigation still works in restricted browsers.
    }
  }, [navCollapsed]);

  useEffect(() => {
    const handleOpenNav = () => setNavCollapsed(false);
    window.addEventListener('open-mobile-nav', handleOpenNav);
    return () => window.removeEventListener('open-mobile-nav', handleOpenNav);
  }, []);

  return (
    <LocationProvider>
      <ApiServiceContext.Provider value={apiService}>
        <div className='bg-base-300 flex h-dvh overflow-hidden'>
          <Navigation
            collapsed={navCollapsed}
            onToggleCollapsed={() => setNavCollapsed(collapsed => !collapsed)}
          />
          <div className='flex min-w-0 flex-1 flex-col overflow-hidden'>
            <div className='flex min-h-0 w-full flex-1 flex-col overflow-auto p-4'>
              <div className='grid min-h-0 flex-1 grid-cols-1'>
                <div className='min-h-0'>
                  <ErrorBoundary>
                    <Router>
                      <Route path='/' component={Home} />
                      <Route path='/dashboard-settings' component={DashboardSettings} />
                      <Route path='/profiles' component={ProfileList} />
                      <Route path='/profiles/:id' component={ProfileEdit} />
                      <Route path='/settings/:tab?' component={Settings} />
                      <Route path='/autotuning' component={AutoTuning} />
                      {ScaleCalibration && (
                        <Route path='/scale-calibration' component={ScaleCalibration} />
                      )}
                      {/* Legacy routes now live in settings tabs */}
                      <Route path='/ota' component={RedirectTo('/settings/system')} />
                      <Route path='/scales' component={RedirectTo('/settings/bluetooth')} />
                      <Route path='/pidtune' component={RedirectTo('/settings/calibration')} />
                      <Route path='/history' component={ShotHistory} />
                      <Route path='/analyzer' component={ShotAnalyzer} />
                      <Route path='/statistics' component={StatisticsPage} />
                      <Route
                        path='/statistics/:sourceAlias/:profileName'
                        component={StatisticsPage}
                      />
                      <Route path='/analyzer/:source/:id' component={ShotAnalyzer} />
                      {/*deep-link route (sorce & ID)*/}
                      <Route default component={NotFound} />
                    </Router>
                  </ErrorBoundary>
                </div>
              </div>
            </div>
            <footer
              className='border-base-300 bg-base-100 flex shrink-0 items-center gap-3 border-t px-4 pt-2 pb-[max(0.5rem,env(safe-area-inset-bottom))]'
              aria-label='Navigation and shot prompts'
            >
              <button
                type='button'
                className='btn btn-circle btn-primary min-h-12 min-w-12 shrink-0 md:hidden landscape:hidden'
                aria-label={navCollapsed ? 'Open menu' : 'Close menu'}
                onClick={() => setNavCollapsed(current => !current)}
              >
                <FontAwesomeIcon icon={faBars} />
              </button>
              <div id='shot-prompt-dock' className='min-w-0 flex-1' />
            </footer>
            <AutoTuningPromptOverlay />
          </div>
        </div>
      </ApiServiceContext.Provider>
    </LocationProvider>
  );
}

// Must be called before render
initializeTheme();

render(<App />, document.getElementById('app'));
