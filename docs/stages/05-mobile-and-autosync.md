# Stage 5: mobile apps and auto-sync

## 1. Goal
Bring the stage 4 app to iOS, iPadOS and Android with:
- native BLE bridges that the OS will wake in the background;
- automatic sync when the puck snaps onto the phone (MagSafe / Pixelsnap);
- transcription and planning scheduled within mobile power limits;
- native calendar and reminder connectors that run in-process.

Actions still run entirely inside the app.

**Exit criterion (from PLAN.md):** snapping the puck on with the app in the background syncs within about 10 s, and actions run in-app on the phone.

## 2. Depends on / unblocks
- **Depends on:** 1 (protocol, advertising states, snap detection), 2 (`mn_link_*` bridge ABI, job queue), 3, 4.
- **Unblocks:** 5b (mobile store builds).

## 3. Deliverables
```
app/ios/Runner/
  MicnoteBle/{BleCentral.swift, L2capLink.swift, RestorationHandler.swift}
  MicnoteBridge.swift          # MethodChannel/FFI glue to core mn_link_*
  BackgroundTasks.swift        # BGProcessingTask / BGAppRefreshTask
  Info.plist                   # UIBackgroundModes, NSBluetoothAlwaysUsageDescription, BGTaskSchedulerPermittedIdentifiers
app/android/app/src/main/kotlin/dev/micnote/
  ble/{BleCentral.kt, L2capLink.kt}
  companion/{PuckCompanionService.kt, Association.kt}
  sync/{SyncForegroundService.kt, TranscribeWorker.kt}
  MicnoteBridge.kt
app/lib/platform/{ble_bridge.dart, background.dart}
app/lib/connectors/calendar/{eventkit.dart, calendar_contract.dart}   # via plugins or pigeon
app/lib/connectors/tasks/{reminders.dart}
docs/measurements/stage5.md
```

## 4. Design detail

### 4.1 Why native BLE (not a Flutter BLE plugin)
Background wake-up depends on OS features that cross-platform plugins don't fully expose:
- iOS Core Bluetooth **state preservation and restoration** (`CBCentralManagerOptionRestoreIdentifierKey`), pending `connect()` calls that never expire, and `CBL2CAPChannel`;
- Android **CompanionDeviceManager** presence events.

So a thin native layer owns the radio and passes raw bytes to core through `mn_link_open/rx/close` and the `link_tx` callback. The protocol logic stays in C++ (one implementation, shared with desktop and the CLI).

**Bridge contract** (Pigeon-generated, type-safe):
```
Dart → native:  startScan(), stopScan(), pair(deviceId), connect(deviceId), disconnect(deviceId)
native → Dart:  onDeviceSeen(id, rssi, syncPending), onLinkOpened(id, linkId), onLinkClosed(id, reason)
native ↔ core:  bytes go straight to libmicnote via FFI from native code (Swift/Kotlin call mn_link_rx directly;
                core's tx callback is registered by native code), avoiding a Dart hop for bulk data
```
Because native code calls core directly, the `mn_context` must be created as early as possible, by native code at app launch (including background relaunch), *before* the Flutter engine runs. Dart then attaches to the same context through `mn_context_shared()`.
**New ABI function:** `mn_context_shared(mn_context** out)` returns the process-wide context. This also covers the case where iOS relaunches the app headless for a BLE event.

### 4.2 iOS / iPadOS
- **Capabilities:**
  - `UIBackgroundModes`: `bluetooth-central`, `processing`, `fetch`.
  - `NSBluetoothAlwaysUsageDescription`: "Micnote connects to your recorder to sync your notes."
- **Auto-sync flow:**
  1. After pairing, store the peripheral identifier. On each launch, call `retrievePeripherals(withIdentifiers:)` and then `connect(peripheral)`. **A pending connect has no timeout.** iOS keeps listening even while the app is suspended.
  2. The puck snaps on, starts ADV_FAST, iOS completes the pending connect and wakes or relaunches the app (restoration via `centralManager(_:willRestoreState:)`).
  3. In `didConnect`: discover the service, read the `psm` characteristic, `openL2CAPChannel(psm)`, `mn_link_open`, and core runs the sync (`Hello`, `SetTime`, list, fetch, ack).
  4. After the sync, disconnect and immediately issue a new pending `connect()` so the next snap is also caught.
  5. Request a `BGProcessingTask` (`requiresExternalPower = false`, `requiresNetworkConnectivity = false`) for transcription. When the user opens the app, foreground processing takes over.
- **Background time budget:** BLE wake events give roughly 10 s of execution per event. That is enough for small recordings. For large ones, keep the link active: each L2CAP receive event extends background execution while data flows (*verify*), and `beginBackgroundTask` is used for the tail of the transfer.
- **Time to sync:** the snap triggers ADV_FAST at a 30 ms interval, so iOS connects within about 1 s of seeing the advertisement. The ≤ 10 s target is mostly limited by iOS's own scan duty cycle for pending connects in the background (*measure*).
- **Termination caveat:** if the user force-quits the app from the switcher, iOS does not relaunch it for BLE events. The onboarding explains this.
- **iPadOS:** the same code. MagSafe only exists on iPhone, so on iPad the puck syncs when the button is pressed (ADV_FAST) or on a timer.
- **Notifications:** "2 new recordings synced · 3 actions to review" (local notifications). Tapping one deep-links to the action queue.

### 4.3 Android (Pixel first, any Android 12+)
- **Pairing:** `CompanionDeviceManager.associate()` with a `BluetoothLeDeviceFilter` (service UUID). This also grants the companion-app privileges (exemptions from background-start restrictions).
- **Presence:** after association, call `startObservingDevicePresence(address)`. Android then binds our `CompanionDeviceService` (`onDeviceAppeared`) when the puck advertises nearby, even if the app is not running. Android 13+ adds `onDevicePresenceEvent` (*verify* the API level details).
- **Auto-sync flow:**
  1. The snap triggers ADV_FAST, which triggers `onDeviceAppeared`.
  2. Start `SyncForegroundService` with `foregroundServiceType="connectedDevice"` and a "Syncing Micnote…" notification.
  3. `connectGatt(autoConnect=false, TRANSPORT_LE)` → request MTU 247 → `PHY_LE_2M` → read PSM → `createInsecureL2capChannel(psm)` (link encryption comes from bonding; *verify* whether `createL2capChannel` (secure) works with our bond).
  4. Core sync, then stop the service.
  5. Enqueue `TranscribeWorker` (WorkManager, `setRequiresBatteryNotLow(true)`, expedited if the app is in the foreground).
- **Permissions:**
  - `BLUETOOTH_SCAN` (with `neverForLocation`), `BLUETOOTH_CONNECT`;
  - `REQUEST_COMPANION_RUN_IN_BACKGROUND`, `REQUEST_COMPANION_USE_DATA_IN_BACKGROUND`, `REQUEST_OBSERVE_COMPANION_DEVICE_PRESENCE`;
  - `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`, `POST_NOTIFICATIONS`.
- **Pixel Battery Share:** when the puck is charging from the phone, the puck reports `charge_source=QI` together with snapped=true (stage 6b). The app may then schedule transcription immediately, since the phone is likely on a charger itself (it doesn't have to be). This is a heuristic only.
- **Doze / OEM battery killers:** companion association exempts us on Pixel. For other OEMs, onboarding links to the system battery-optimization settings (an in-app intent to settings, which is a system screen and not an action).

### 4.4 Mobile model sizing and scheduling
| Device class | ASR model | LLM | Planning |
|---|---|---|---|
| ≥ 8 GB RAM (recent Pixel Pro, iPhone Pro) | `small.en` q5 | 3–4B Q4 | on-device, when charging or when the app is in the foreground |
| 6 GB | `base.en` q5 | 3B Q4 or cloud opt-in | as above |
| < 6 GB | `base.en` q5 | cloud opt-in only, or defer to a paired desktop | — |

- **Thermal and battery:** whisper.cpp on iOS uses Metal (plus Core ML for the encoder if that is faster in measurements). Android uses CPU NEON with 4 threads.
- **Pause rules:** work pauses when the battery is below 20% and not charging, or when the thermal state is `serious` or worse (iOS `ProcessInfo.thermalState`, Android `PowerManager.getCurrentThermalStatus`).
- **Deferring to a desktop:** this needs the host-to-host sync from PLAN.md (mDNS + Noise_XX, end-to-end encrypted). It is *post-v1*. Until then, the "defer" option simply means "process when the app is opened while charging".

### 4.5 Native calendar and reminders (in-process)
| Platform | Calendar | Reminders/Tasks | Access |
|---|---|---|---|
| iOS/macOS | EventKit `EKEventStore.save(_:span:)` | EventKit reminders | `requestFullAccessToEvents` / `requestFullAccessToReminders` (iOS 17+), or write-only access for events (*verify* whether write-only is enough: we also read busy times for conflict hints) |
| Android | `CalendarContract.Events` insert via ContentResolver | none built in, so Google Tasks API or Microsoft To Do via OAuth | `READ_CALENDAR`/`WRITE_CALENDAR` |

- **Implementation:** Pigeon interfaces implemented natively, so we control the exact fields. Prefer this over third-party plugins.
- **Editors:** the same as the desktop editors from stage 4. The account picker lists the device calendars.
- **Undo:** delete the event or reminder by its stored identifier.

### 4.6 Mobile UI adjustments
- Stacked navigation: Inbox → Recording → Item → Action editor (full screen).
- PR review on a phone: unified diff only, with comment cards inline. A tablet gets the split view.
- Confirm sheet as a modal bottom sheet. Hold-to-confirm with haptic feedback.
- Pairing on iPhone: "Snap the puck on, then hold its button for 5 s." The RSSI gate (stage 1, §4.6) is satisfied naturally when the puck is snapped on.

### 4.7 Measurements to record (`docs/measurements/stage5.md`)
- Time from snap to sync start and to sync complete, for 1 min / 10 min / 60 min recordings, with the app in the foreground, backgrounded, and suspended for 1 h (iPhone + Pixel).
- Battery cost per hour of audio processed on each phone.
- Transcription real-time factor per model on each phone.
- L2CAP throughput (iOS vs Android) compared with stage 1's numbers.

## 5. Task breakdown
**PR 1: shared context and bridge**
- [ ] M: `mn_context_shared`, native-side context bootstrap, Pigeon bridge definitions
- [ ] S: core `link_tx` callback registration for native code

**PR 2: iOS BLE**
- [ ] L: `BleCentral` with restoration, pending connects, L2CAP channel, GATT fallback
- [ ] M: background tasks + notifications
- [ ] S: Info.plist and entitlements

**PR 3: Android BLE**
- [ ] L: CDM association + presence service + foreground sync service
- [ ] M: L2CAP channel, MTU/PHY, GATT fallback
- [ ] M: WorkManager transcription and planning

**PR 4: models on mobile**
- [ ] M: device-class detection + model choice + pause rules
- [ ] M: build whisper.cpp/llama.cpp for iOS (Metal) and Android (NDK, arm64-v8a) in the Flutter build

**PR 5: native connectors**
- [ ] M: EventKit calendar + reminders (iOS)
- [ ] M: CalendarContract (Android) + the Google Tasks connector
- [ ] S: mobile OAuth via `flutter_appauth`

**PR 6: UI and tests**
- [ ] M: mobile layouts, unified diff, bottom sheets
- [ ] M: measurement sessions + report

## 6. Testing and verification
- **Unit:** Kotlin and Swift tests for the bridge framing; a core `mn_link_*` test with a loopback transport.
- **On-device scripted tests** (manual checklist, run before each release):
  1. Pair through the SAS flow with the puck snapped on.
  2. Background the app, lock the phone, record a 2-minute memo on the puck with it detached, snap it back on. A notification arrives within about 10 s; open the app and the transcript and actions are there (or being processed).
  3. Repeat after a reboot of the phone, without opening the app (Android: the presence service must still fire; iOS: needs one launch after reboot, which is documented).
  4. Force-quit (iOS): confirm the documented behavior.
  5. Run `send_email` and `create_event` fully in-app on the phone.
- **Automated:** `flutter test`; Android instrumented tests for permission flows using a mock CDM where feasible; iOS XCTest for restoration-state handling with simulated `willRestoreState` dictionaries.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| iOS background sync slower than 10 s | Measure; tune the puck's ADV_FAST timing; keep sending the button-press advertising burst as a manual trigger |
| Force-quit apps can't be woken on iOS | Onboarding copy; a "last synced" indicator on the puck status |
| Android OEM background restrictions | CDM exemptions; per-OEM guidance; Pixel is the primary target |
| Large models too slow or hot on phones | Smaller defaults; cloud opt-in; process while charging |
| Running native code before the Flutter engine adds complexity | A single `mn_context_shared` design, tested with a headless launch on both OSes |

## 8. Open questions
- Is EventKit write-only access enough? It depends on whether conflict hints are worth the extra permission.
- Should the iPad and Android tablet layouts share the desktop three-pane UI? Probably yes, via adaptive breakpoints.
- Host-to-host sync (desktop does the heavy processing for phones): schedule it as a post-v1 stage.

## 9. Out of scope
- App Store and Play distribution mechanics (stage 5b).
- Puck firmware changes beyond tuning advertising (stage 1 owns the protocol).
