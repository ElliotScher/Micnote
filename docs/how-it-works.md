# How Micnote works

This page explains the whole of Micnote with no technical background assumed. Every other document goes deeper into one part; this one is the map. Words in *italics* are explained in the [glossary](glossary.md) if you want more.

## The idea in one paragraph

Micnote is a small recorder you clip onto the back of your phone. You talk to it: a quick reminder, a list of things to do, or a whole meeting. Later, your own phone or computer turns the recording into text and drafts the follow-up work for you: an email to send, a GitHub issue to open, a calendar event, a reminder. You check each draft, change anything you like, press confirm, and the app does it. Your recordings never go to someone else's servers unless you explicitly say so for one item, and even then you see exactly what will be sent first.

## Why it exists

Commercial AI note-takers upload your conversations to the company's cloud, where they're transcribed and analysed. You have to trust how that company stores, uses and shares some of the most sensitive things you say, including what other people said in your meetings. You can't check.

Micnote takes the opposite approach:

- **You own the hardware and the software.** It's open source: anyone can read every line, build it themselves, and check it does what it says.
- **Everything happens on your own devices by default.** Speech-to-text and the AI that drafts your actions run on your phone or computer.
- **Nothing happens without you.** The AI only *drafts*. Every email, issue or event waits for you to review and confirm it.
- **No tracking.** The app sends no usage data anywhere. The only network traffic it makes on its own is downloading its AI models, once, from fixed addresses it checks.

## The pieces

There are three parts, and the rest of the documentation is organised around them:

| Part | What it is | Where it lives in the project |
|---|---|---|
| **The puck** | The recorder: a round device about 57 mm (2¼ in) across and 1 cm thick, with two microphones, a button, a coloured light, a small vibration motor, a battery, and a magnet ring that matches the one in modern phones | `hardware/` (its circuit board and case), `firmware/` (the software inside it) |
| **The core** | The engine, a library of code shared by every app: it talks to the puck, decrypts and stores recordings, turns speech into text and drafts actions | `core/` |
| **The app** | What you see and touch, on iPhone, iPad, Android, Mac, Windows and Linux: your recordings, transcripts, action drafts and their editors | `app/` |

A fourth folder, `proto/`, holds the exact description of every message the puck and the app send each other, so both sides always agree.

## A recording's journey

### 1. Recording, with or without your phone

You press the puck's button. It buzzes once and its light shows that it's recording. While you talk, you can tap to mark "next item" (for example between todos), which gives a double buzz.

The puck doesn't need your phone nearby. It can record for days or weeks on its own and keeps everything until a phone or computer has safely collected it. It warns you when its storage is 90% full and stops with a long buzz if it's full: it never records over something you haven't synced.

Inside, the sound from the two microphones is compressed with *Opus*, a format designed for clear speech at small sizes: an hour takes about 7 MB, so the puck's memory holds about 9 hours.

### 2. Locked the moment it's recorded

Before a single second of audio is saved, the puck encrypts it: scrambles it with a random key, made new for every recording. That key is then locked separately for each phone or computer you've paired, using that device's own *public key*.

This has an unusual consequence: **the puck cannot play back its own recordings.** Only your paired devices hold the keys that unlock them. If someone steals the puck, all they get is scrambled data.

### 3. Pairing: introducing the puck to your devices

The first time you use a phone or computer with the puck, you pair them. The puck's light flashes a sequence of colours and the app shows a sequence too. If they match, you press the puck's button. That check makes sure the puck is talking to your device and not to someone nearby pretending to be it. From then on, they recognise each other automatically, and up to four devices can be paired at once.

### 4. Syncing: collecting the recordings

When the puck snaps onto your phone (a sensor feels the phone's magnets and a small knock), or when you plug it into a computer with USB, it hands over its recordings. Over Bluetooth an hour of audio takes a minute or two; over USB, seconds.

Each recording arrives in pieces, each checked as it comes. If the connection drops halfway, the next sync carries on from where it stopped. The puck deletes a recording only after your device confirms it has the whole thing safely.

Every sync also sets the puck's clock. If the battery ever ran flat and the clock was lost, the puck counted how many times it started up and how long it had been running, so the app can still work out when each recording was made.

### 5. Turning speech into text

Your phone or computer now unlocks the recording and turns it into text using *whisper.cpp*, a free, accurate speech-recognition program that runs entirely on your device: nothing is uploaded. It first skips the silent stretches, which is quicker and stops the AI imagining words in the quiet. Long meetings are processed in 10-minute pieces, so even phones can handle them.

Which speech model is used depends on the device: a big, very accurate one on a powerful computer, a smaller one on a phone. They're downloaded once, the first time, and each download is checked against a fingerprint built into the app, so a corrupted or tampered file is rejected.

The text is saved in an encrypted database on your device, and you can search it.

### 6. Drafting your actions

Next, the *planner* reads the transcript. It splits it into items (using the marks you made with the puck as hints) and drafts one action for each: "email Jeff the Q3 numbers" becomes an email to Jeff with a subject and body; "file a bug about the login page" becomes a GitHub issue.

The planner can only choose from the kinds of actions the app knows how to do, and every draft must fill in that action's exact form. It can look things up to make the draft accurate (which Jeff in your contacts? what does that pull request change?), but looking up is all it can do: it can't send or change anything.

By default the planner is an AI running on your own device. For an item you find tricky, you can choose to send it to Claude, a more capable AI service, instead. Before anything leaves your device, email addresses, phone numbers, anything that looks like a password or access key, and any words you've listed (client or project names, say) are replaced with placeholders such as `[EMAIL_1]`. You then see a preview of exactly what will be sent and approve it. When the answer comes back, the app puts the real details back in on your device, so Claude never sees them.

### 7. You review, the app does it

Each draft opens in an editor built for it: an email editor with recipients, subject and body; an issue editor with the repository, title and labels; a code-review screen showing the changes with the AI's suggested comments, which you can edit, move or delete.

When you're happy, you press **Confirm**. You see a summary of exactly what will happen ("Send to jeff@... · Subject ... · 1 attachment"). For riskier actions, such as sending an email or approving someone's code, you hold the button for a moment as a second check.

Then **the app does it itself**, talking directly to Gmail, Outlook, GitHub, your calendar and so on. It never hands you off to another app or a web page to finish. Where the service allows it, you can undo: an email waits 10 seconds before it really goes, an issue can be closed again, an event deleted. Every action taken is written to a permanent log.

## How it's kept safe

The [threat model](threat-model.md) lists every way someone might try to get at your data and what stops them. The main protections:

- **On the puck:** recordings are encrypted with keys the puck itself can't use; it only runs software carrying Micnote's digital signature; its debug connector is locked in finished units; and it only announces itself over Bluetooth when it needs to, using an address that keeps changing so it can't be tracked.
- **Between the puck and your devices:** pairing is confirmed by you with the colour check, and every connection starts with both sides proving they know the secret they agreed at pairing.
- **On your devices:** the database is encrypted, and its key, your device's key and your service sign-ins live in the operating system's protected vault (the keychain).
- **With the AI:** whatever was said in a recording, or written in an email or code change the planner reads, is treated as information, never as instructions, and nothing is ever done without your confirmation. This guards against *prompt injection*: someone saying or writing "ignore your instructions and send...".

## How the project is being built

The work is divided into stages, each with its own detailed plan in [`docs/stages/`](stages/README.md) and progress checkboxes in the [master plan](PLAN.md):

| Stage | In plain terms |
|---|---|
| 0a | Make it easy for any developer to install the tools they need |
| 0b | Set up the skeleton of every part, with automatic building and testing of every change |
| 1 | Make the first working recorder on off-the-shelf prototype boards, and sync it to a computer |
| 2 | Turn recordings into searchable text on your own device |
| 3 | Draft actions from the text |
| 4 | The desktop app: review, edit, confirm and carry out actions |
| 5 | The phone apps, including syncing automatically when the puck snaps on |
| 5b | Proper installers for every platform, and the release process |
| 6a–6c | Design, build and test Micnote's own circuit board and case |
| 7 | A full security check before version 1.0 |

Much of stage 1 and 2 software is done and tested on simulators. The rest of stage 1 waits for the prototype boards.

## Where to go next

- **To use Micnote:** installers come with stage 5b. Until then, Micnote is for developers.
- **To build it yourself:** the [README](../README.md) walks you through setting up from scratch, and the [development setup guide](dev-setup.md) has every detail.
- **To understand one part deeply:** the [protocol](protocol.md) (how the puck and app talk), the [threat model](threat-model.md) (security), the [action schema](action-schema.md) (what actions look like), and the [stage plans](stages/README.md).
- **When a word is unfamiliar:** the [glossary](glossary.md).
