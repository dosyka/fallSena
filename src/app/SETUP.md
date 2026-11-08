# FallSena — Firebase + Flutter Setup Runbook (owner)

This is the ordered, owner-run setup for the FallSena mobile backend. It covers
the console steps only you can do (they need your Google account and billing),
plus the local tooling to wire the Flutter app to Firebase and deploy the
security rules in `firestore.rules`.

**Scope of this pass:** Android + Firebase backend. iOS app registration and
APNs are deferred to a later iOS bring-up pass (noted at the end).

Prerequisites: a Google account, a credit/debit card for the Blaze plan (stays
$0 within the free quota), and a terminal.

---

## 1. Create the Firebase project

1. Go to <https://console.firebase.google.com> → **Add project**.
2. Name it (e.g. `fallsena`). Accept the terms.
3. Google Analytics is optional — you can disable it for now.
4. Wait for provisioning, then **Continue**.

## 2. Upgrade to the Blaze (pay-as-you-go) plan

The notification relay uses Cloud Functions (`events` onCreate → FCM), which
requires Blaze. It stays $0 within the generous free tier.

1. In the console, bottom-left → **Upgrade** / project **Settings → Usage and
   billing**.
2. Select **Blaze**, link or create a billing account.
3. (Recommended) Set a **budget alert** so you're notified before any spend.

## 3. Enable Firestore (Native mode)

1. Console → **Build → Firestore Database → Create database**.
2. Choose **Native mode** (NOT Datastore mode).
3. Pick a location close to your users (this is permanent).
4. Start in **production mode** (locked). The rules in `firestore.rules` will be
   deployed in step 9 — do not leave it in test mode.

## 4. Enable Authentication (Email/Password)

1. Console → **Build → Authentication → Get started**.
2. **Sign-in method** tab → enable **Email/Password** → Save.
   (Other providers can be added later; the app targets Email/Password first.)

## 5. Register the Android app

1. Console → project **Overview** → **Add app** → **Android**.
2. **Android package name:** `com.fallsena.app`  (must match exactly).
3. App nickname (optional): `FallSena Android`.
4. SHA-1 is optional now (needed later only for some sign-in features / Dynamic
   Links) — you can add it afterward from the app settings.
5. **Download `google-services.json`.**
6. Place it at: `src/app/android/app/google-services.json`.
   - This path exists after the Flutter app is scaffolded (step 7). If you run
     `flutterfire configure` (step 8), it also writes/refreshes this file for
     you, so downloading it by hand is optional — but this is where it must live.
   - `google-services.json` is gitignored by the standard Flutter `.gitignore`;
     keep it out of version control.

## 6. iOS — deferred

iOS app registration (bundle id `com.fallsena.app`), `GoogleService-Info.plist`,
the APNs auth key, and Push Notifications capability are handled in a later
**iOS bring-up pass** (requires a Mac + Xcode + Apple Developer Program, $99/yr).
Skip iOS for now.

---

## 7. Install the local toolchain

Install these on your dev machine:

1. **Flutter SDK** — <https://docs.flutter.dev/get-started/install>. After
   install, verify and clear any setup issues:
   ```bash
   flutter --version
   flutter doctor
   ```
2. **Node.js + npm** (for the Firebase CLI) — <https://nodejs.org>.
3. **Firebase CLI** (`firebase-tools`):
   ```bash
   npm install -g firebase-tools
   firebase login
   ```
4. **FlutterFire CLI**:
   ```bash
   dart pub global activate flutterfire_cli
   ```
   Make sure the pub global bin dir is on your `PATH`
   (`~/.pub-cache/bin` on macOS/Linux) so `flutterfire` is found.

> If the Flutter app skeleton has not been scaffolded yet, create it now from
> inside `src/app/`:
> ```bash
> flutter create --org com.fallsena --project-name fallsena .
> ```
> then add the deps `firebase_core`, `firebase_auth`, `cloud_firestore`,
> `firebase_messaging` to `pubspec.yaml` and run `flutter pub get`.

## 8. Wire Firebase into the Flutter app (`flutterfire configure`)

This is **your** step — it uses your Google account to pull the project config
and generate `lib/firebase_options.dart` (which is intentionally not committed
and not created by the implementer).

From the app directory `src/app/`:
```bash
flutterfire configure --project=<your-firebase-project-id>
```
- Select the `fallsena` project.
- Select platforms: **android** now (add **ios** during the iOS bring-up pass).
- Confirm the Android package name `com.fallsena.app`.

This generates `src/app/lib/firebase_options.dart` and refreshes
`android/app/google-services.json`. After it exists, replace the commented TODO
placeholder in `lib/main.dart` with the real initialization:
```dart
import 'firebase_options.dart';
// ...
await Firebase.initializeApp(
  options: DefaultFirebaseOptions.currentPlatform,
);
```

## 9. Deploy the Firestore security rules

The rules live at `src/app/firestore.rules`. Point the Firebase CLI at them,
then deploy.

1. If you have not initialized Firebase config in the app dir, either run
   `firebase init firestore` (choose the existing project; when asked for the
   rules file, point it at `firestore.rules`), or create a minimal
   `firebase.json` in `src/app/`:
   ```json
   {
     "firestore": {
       "rules": "firestore.rules"
     }
   }
   ```
2. Deploy only the rules:
   ```bash
   firebase deploy --only firestore:rules --project=<your-firebase-project-id>
   ```
3. Verify in the console: **Firestore → Rules** should show the deployed
   content matching `firestore.rules`.

> Tip: to test rules locally before deploying, install the emulator suite
> (`firebase init emulators` → Firestore) and run `firebase emulators:start`.

---

## 10. What comes next (not in this pass)

- App auth code (sign-up/sign-in) — built after `firebase_options.dart` exists.
- The `events` onCreate Cloud Function that sends the FCM push (backlog #P3-B2).
- iOS bring-up: register the iOS app, add `GoogleService-Info.plist`, upload the
  APNs auth key in the console, enable Push Notifications in Xcode.

## Quick checklist

- [ ] Firebase project created
- [ ] Blaze plan enabled (+ budget alert)
- [ ] Firestore in **Native mode**, production rules
- [ ] Auth: Email/Password enabled
- [ ] Android app registered (`com.fallsena.app`), `google-services.json` in
      `android/app/`
- [ ] Flutter + `flutterfire_cli` + `firebase-tools` installed
- [ ] `flutterfire configure` run → `lib/firebase_options.dart` generated
- [ ] `firebase deploy --only firestore:rules` succeeded
