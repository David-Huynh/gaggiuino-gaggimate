import { grindAvailable } from '../../utils/grind.js';
import { displayedTemperatureTarget } from '../../utils/temperatureTarget.js';
import { computed } from '@preact/signals';
import { useCallback, useContext, useEffect, useState } from 'preact/hooks';
import { useQuery } from 'preact-fetching';
import { ApiServiceContext, machine } from '../../services/ApiService.js';
import { MODES } from './utils.js';
import { t } from '@lingui/core/macro';
import { computeWaterLevel } from '../../utils/waterLevel.js';
import { brewStartErrorMessage } from '../../utils/brewStartError.js';

const status = computed(() => machine.value.status);
const capabilities = computed(() => machine.value.capabilities);
const connected = computed(() => machine.value.connected);

export function useDashboardState() {
  const apiService = useContext(ApiServiceContext);
  const [localOptimizationEnabled, setLocalOptimizationEnabled] = useState(true);
  const [sendError, setSendError] = useState('');
  const [flushPending, setFlushPending] = useState(false); // flush requested, status not caught up yet

  const s = status.value;
  const caps = capabilities.value;
  const p = s.process;

  // Anything but a ready controller locks the dashboard into standby, exactly like the touch UI.
  const systemReady = connected.value && (!s.system || s.system.state === 'ready');
  // Only a not-ready state carries a message; ready leaves the default standby text in place.
  const systemMessage = !connected.value
    ? t`Connecting to the machine...`
    : systemReady
      ? ''
      : s.system.message || s.system.state;
  const mode = systemReady ? s.mode : 0;

  const { data: settings } = useQuery(
    'settings-cache',
    async () => (await fetch('/api/settings')).json(),
    { staleTime: 30000, refetchOnWindowFocus: false },
  );

  useEffect(() => {
    if (settings?.rlLocalOptimizationEnabled !== undefined) {
      setLocalOptimizationEnabled(!!settings.rlLocalOptimizationEnabled);
    }
  }, [settings?.rlLocalOptimizationEnabled]);

  // ── derived ──────────────────────────────────────────────
  const isActive = !!p?.a;
  const isStarting = !!s.brewStartPending;
  const isFinished = p?.e != null && !isActive;
  const isBrewing = mode === 1;
  const isGrinding = mode === 4;
  // The firmware flags a flush (utility process) so this state survives re-renders and reloads.
  const flushRunning = !!p?.u && isActive;
  const flushFinished = !!p?.u && isFinished;
  const isFlushing = flushPending || flushRunning;

  const isSmartGrindEnabled = settings?.smartGrindActive || false;
  const altRelayFunction = settings?.altRelayFunction ?? 1;
  const isGrindAvailable = grindAvailable(settings, caps);
  const showGrindTab = isGrindAvailable || isGrinding;
  // Dual boilers steam and dispense water from brew mode; keep a hidden tab while it is the current mode.
  const steamWaterAvailable = !caps?.dualBoiler;
  const visibleModes = MODES.filter(m => {
    if (m.id === mode) return true;
    if (m.id === 4) return showGrindTab;
    if (m.id === 2 || m.id === 3) return steamWaterAvailable;
    return true;
  });

  // ── water level (Alba) ────────────────────────────────────
  // The tank card follows an Alba with its percentage sensor; null means "attached but not calibrated".
  const ledControl = caps?.ledControl || false;
  const albaWaterLevelAvailable = !!s.albaWaterLevelAvailable;
  const waterLevelPercent = albaWaterLevelAvailable
    ? computeWaterLevel({
        empty: settings?.emptyTankDistance,
        full: settings?.fullTankDistance,
        distance: s.tofDistance,
      })
      : null;

  // ── handlers ─────────────────────────────────────────────
  const send = useCallback(tp => {
    setSendError('');
    try {
      apiService.send({ tp });
    } catch {
      setSendError('Connection lost. Wait for reconnection, then try again.');
    }
  }, [apiService]);

  const toggleSteam = () => {
    if (systemReady && caps.dualBoiler && mode !== 0 && s.steamControlsAvailable !== false)
      send('req:steam:toggle');
  };

  const changeMode = newMode => {
    if (!systemReady) return; // locked in standby until the controller is ready
    if (caps.dualBoiler && newMode === 2) {
      toggleSteam();
      return;
    }
    apiService.send({ tp: 'req:change-mode', mode: newMode });
  };

  const activate = () => send(isGrinding ? 'req:grind:activate' : 'req:process:activate');
  const deactivate = () => send(isGrinding ? 'req:grind:deactivate' : 'req:process:deactivate');
  const clear = () => {
    send('req:process:clear');
    setFlushPending(false);
  };

  const startFlush = () => {
    if (isFlushing) return;
    setFlushPending(true);
    apiService.request({ tp: 'req:flush:start' }).catch(() => setFlushPending(false));
  };
  // Ends a hold-to-flush (flush duration 0); the firmware ignores it for a fixed-length flush.
  const stopFlush = () => {
    if (!isFlushing) return;
    send('req:flush:stop');
  };

  // Let the firmware confirm the flush end before clearing its summary.
  useEffect(() => {
    if (flushRunning || flushFinished) setFlushPending(false);
    if (flushFinished) send('req:process:clear');
  }, [flushRunning, flushFinished, send]);

  const raiseTemp = () => send('req:raise-temp');
  const lowerTemp = () => send('req:lower-temp');
  const raiseTarget = () => send(isGrinding ? 'req:raise-grind-target' : 'req:raise-brew-target');
  const lowerTarget = () => send(isGrinding ? 'req:lower-grind-target' : 'req:lower-brew-target');
  const changeTarget = target =>
    apiService.send({
      tp: isGrinding ? 'req:change-grind-target' : 'req:change-brew-target',
      target,
    });
  const toggleLocalOptimization = () => {
    const enabled = !localOptimizationEnabled;
    setLocalOptimizationEnabled(enabled);
    apiService.send({ tp: 'req:rl:local-optimization', enabled });
  };

  const displayedTarget = displayedTemperatureTarget(
    s.targetTemperature,
    s.controllerTargetTemperature,
  );

  return {
    brewStartError: sendError || brewStartErrorMessage(s.brewStartError),
    // raw status
    smartDose: s.smartDose,
    albaWaterLevelAvailable,
    hasTankLevel: !!s.hasTankLevel,
    tankLevelOK: s.tankLevelOK ?? null,
    hasSteamLevel: !!s.hasSteamLevel,
    steamBoilerLow: s.steamBoilerLow ?? null,
    mode,
    systemReady,
    systemMessage,
    currentTemperature: s.currentTemperature,
    targetTemperature: s.targetTemperature,
    currentSteamTemperature: s.currentSteamTemperature,
    targetSteamTemperature: s.targetSteamTemperature,
    steamActive: !!s.steamActive,
    steamControlsAvailable: s.steamControlsAvailable !== false,
    displayedTargetTemperature: displayedTarget.value,
    controllerTargetMismatch: displayedTarget.mismatch,
    currentPressure: s.currentPressure,
    targetPressure: s.targetPressure,
    currentFlow: s.currentFlow,
    targetFlow: s.targetFlow,
    currentWeight: s.currentWeight,
    currentPumpPower: s.currentPumpPower ?? 0,
    currentBoilerPower: s.currentBoilerPower ?? 0,
    currentPuckResistance: s.currentPuckResistance ?? 0,
    currentPuckFlow: s.currentPuckFlow ?? 0,
    currentCoffeeVolume: s.currentCoffeeVolume ?? 0,
    targetWeight: s.targetWeight,
    volumetricAvailable: s.volumetricAvailable,
    brewTarget: !!s.brewTarget,
    grindTarget: s.grindTarget,
    grindTargetDuration: s.grindTargetDuration,
    grindTargetVolume: s.grindTargetVolume,
    selectedProfile: s.selectedProfile,
    selectedProfileId: s.selectedProfileId,
    processInfo: p,
    tofDistance: s.tofDistance,
    warnings: s.warnings ?? [],
    dualBoiler: caps.dualBoiler,
    // derived
    isActive,
    isStarting,
    isFinished,
    isBrewing,
    isGrinding,
    isGrindAvailable,
    showGrindTab,
    visibleModes,
    // water level
    ledControl,
    waterLevelPercent,
    // settings
    isSmartGrindEnabled,
    altRelayFunction,
    autoTuningEnabled: !!settings?.rlAutoTuningEnabled,
    localOptimizationEnabled,
    beanContextName: settings?.rlBeanContextName || '',
    hasBeanContext: !!settings?.rlBeanContextId,
    // action state
    isFlushing,
    // handlers
    changeMode,
    toggleSteam,
    activate,
    deactivate,
    clear,
    startFlush,
    stopFlush,
    raiseTemp,
    lowerTemp,
    raiseTarget,
    lowerTarget,
    changeTarget,
    toggleLocalOptimization,
  };
}
