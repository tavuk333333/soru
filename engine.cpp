#Requires AutoHotkey v2.0
#SingleInstance Force
SetWorkingDir A_ScriptDir

; Disable AHK's built-in "X hotkeys have been received in the last Yms"
; flood-warning dialog. This script intentionally fires hotkeys/sends very
; rapidly (Z-Boost, Click Boost, the spammer, MacroLoop), which trips that
; safety check by design — it's not a bug, so we just turn the popup off.
A_HotkeyInterval := 0

; Explicit screen coordinates everywhere — used by the coordinate picker's
; MouseGetPos/PixelGetColor calls and its ToolTip so the tip lands right
; next to the cursor instead of being anchored to some window.
CoordMode "Mouse", "Screen"
CoordMode "Pixel", "Screen"
CoordMode "ToolTip", "Screen"

; ══════════════════════════════════════════════════════════════
;  THEME COLORS
; ══════════════════════════════════════════════════════════════
global BgColor := "1e1e1e"
global BarColor := "151515"
global ActiveTabColor := "2d2d2d"
global TintColor := "800080"
global TextMain := "ffffff"
global DimText := "888888"
global DividerColor := "333333"

; ══════════════════════════════════════════════════════════════
;  THEMED CHECKBOX (custom-drawn, matches the dark UI)
; ══════════════════════════════════════════════════════════════
class ThemedCheckbox {
    __New(guiObj, x, y, w, text, checked := false) {
        global TintColor, ActiveTabColor, TextMain

        boxY := y - 1   ; box nudged up 1px; label stays put

        this.checked   := checked
        this.callbacks := []

        this.box := guiObj.Add("Text",
            "x" x " y" boxY " w16 h16 +0x200 +Border Center Background" (checked ? TintColor : ActiveTabColor),
            checked ? "✓" : "")
        this.box.SetFont("s9 Bold c" TextMain, "Segoe UI")

        this.label := guiObj.Add("Text",
            "x" (x + 24) " y" (y - 1) " w" (w - 24) " h18 +BackgroundTrans c" TextMain,
            text)
        this.label.SetFont("s9 c" TextMain, "Segoe UI")

        this.box.OnEvent("Click", (*) => this.Toggle())
        this.label.OnEvent("Click", (*) => this.Toggle())
    }

    Toggle() {
        this.Value := !this.checked
    }

    Refresh() {
        global TintColor, ActiveTabColor
        this.box.Opt(this.checked ? "Background" TintColor : "Background" ActiveTabColor)
        this.box.Text := this.checked ? "✓" : ""
    }

    OnEvent(name, cb) {
        if (name = "Click")
            this.callbacks.Push(cb)
    }

    Value {
        get => this.checked ? 1 : 0
        set {
            this.checked := value ? true : false
            this.Refresh()
            for cb in this.callbacks
                cb.Call(this)
        }
    }
}

; ══════════════════════════════════════════════════════════════
;  BOOST MODULE  (shared engine behind both Z-Boost and Click Boost)
;  Two independent instances of this class are created below —
;  they have separate hotkeys, separate settings, separate
;  counters, and can both run at the same time.
; ══════════════════════════════════════════════════════════════
class BoostModule {
    static QPCFreq := 0

    static Init() {
        freq := 0
        DllCall("QueryPerformanceFrequency", "Int64*", &freq)
        BoostModule.QPCFreq := freq
    }

    ; GetAsyncKeyState's low bit means "pressed since the last check" — the
    ; click used to open a rebind box still shows up as "recently pressed"
    ; a moment later, so it gets mistaken for the new key. Calling it once
    ; for every key clears that history right before polling starts.
    static DrainKeyState() {
        Loop 254
            DllCall("GetAsyncKeyState", "Int", A_Index, "Short")
    }

    static PreciseSleep(ms) {
        ticks := ms * BoostModule.QPCFreq / 1000
        start := 0
        DllCall("QueryPerformanceCounter", "Int64*", &start)
        now := start
        while (now - start < ticks)
            DllCall("QueryPerformanceCounter", "Int64*", &now)
    }

    static SuspendProcess(pid) {
        hProcess := DllCall("OpenProcess", "UInt", 0x0800, "Int", 0, "UInt", pid, "Ptr")
        if hProcess {
            DllCall("ntdll\NtSuspendProcess", "Ptr", hProcess)
            DllCall("CloseHandle", "Ptr", hProcess)
        }
    }

    static ResumeProcess(pid) {
        hProcess := DllCall("OpenProcess", "UInt", 0x0800, "Int", 0, "UInt", pid, "Ptr")
        if hProcess {
            DllCall("ntdll\NtResumeProcess", "Ptr", hProcess)
            DllCall("CloseHandle", "Ptr", hProcess)
        }
    }

    __New(id, displayName, defaultKey, useClick, processName) {
        this.id           := id            ; config section name, e.g. "ZBoost" / "ClickBoost"
        this.displayName  := displayName   ; shown in messages, e.g. "Z-Boost" / "Click Boost"
        this.useClick     := useClick      ; true = sends a left click, false = sends a key
        this.processName  := processName
        this.other         := ""           ; the *other* module, set after both exist

        this.boostKey     := defaultKey
        this.defaultKey   := defaultKey
        this.zKey         := "z"           ; only meaningful when useClick = false
        this.holdTime     := 50
        this.lagStart     := 150
        this.lagDuration  := 400
        this.intensity    := 4
        this.enabled      := true

        this.boostActive    := false
        this.throttleActive := false
        this.rebinding      := false
        this.rebindTarget   := ""
        this.boostCount     := 0
        this.sessionStart   := A_TickCount
        this.freezeTime     := 0
        this.runTime        := 1
        this.hotkeyRegistered := false

        ; Bound once so the exact same function object can be registered/
        ; unregistered later (HotKey/SetTimer need identity to match).
        this.triggerFn := ObjBindMethod(this, "TriggerBoost")
        this.pollFn    := ObjBindMethod(this, "PollForKey")
    }

    CalcTimings() {
        this.freezeTime := Round(this.intensity * 5)
        this.runTime    := 1
    }

    RegisterHotkey() {
        try HotKey "~" this.boostKey, this.triggerFn, "On"
        this.hotkeyRegistered := true
    }

    UnregisterHotkey() {
        if !this.hotkeyRegistered
            return
        try HotKey "~" this.boostKey, this.triggerFn, "Off"
        this.hotkeyRegistered := false
    }

    ; ── SETTINGS I/O ─────────────────────────────────────────
    SaveConfig() {
        global ConfigFile
        s := this.id
        IniWrite this.boostKey,             ConfigFile, s, "BoostKey"
        if (!this.useClick)
            IniWrite this.zKey,             ConfigFile, s, "ZKey"
        IniWrite this.holdTime,             ConfigFile, s, "HoldTime"
        IniWrite this.lagStart,             ConfigFile, s, "LagStart"
        IniWrite this.lagDuration,          ConfigFile, s, "LagDuration"
        IniWrite this.intensity,            ConfigFile, s, "Intensity"
        IniWrite (this.enabled ? 1 : 0),    ConfigFile, s, "Enabled"
    }

    LoadConfig() {
        global ConfigFile
        if !FileExist(ConfigFile)
            return
        s := this.id
        this.boostKey    := IniRead(ConfigFile, s, "BoostKey", this.boostKey)
        if (!this.useClick)
            this.zKey    := IniRead(ConfigFile, s, "ZKey", this.zKey)
        this.holdTime    := Integer(IniRead(ConfigFile, s, "HoldTime",    this.holdTime))
        this.lagStart    := Integer(IniRead(ConfigFile, s, "LagStart",    this.lagStart))
        this.lagDuration := Integer(IniRead(ConfigFile, s, "LagDuration", this.lagDuration))
        this.intensity   := Integer(IniRead(ConfigFile, s, "Intensity",   this.intensity))
        this.enabled     := Integer(IniRead(ConfigFile, s, "Enabled", this.enabled ? 1 : 0)) ? true : false
    }

    SyncUIFromState() {
        this.keyBox.Value := StrUpper(this.boostKey)
        if (!this.useClick)
            this.zKeyBox.Value := StrUpper(this.zKey)
        this.holdEdit.Value      := this.holdTime
        this.lagStartEdit.Value  := this.lagStart
        this.lagDurEdit.Value    := this.lagDuration
        this.intensityEdit.Value := this.intensity
        this.enabledCB.Value     := this.enabled ? 1 : 0
        this.statusBar.Value     := "Ready — press " StrUpper(this.boostKey) " to boost"
    }

    ; ── UI EVENT HANDLERS ────────────────────────────────────
    ToggleEnabled(ctrl, *) {
        this.enabled := ctrl.Value ? true : false
        SaveAllConfig()
    }

    ApplySettings(*) {
        global TintColor
        try {
            newIntensity := Integer(this.intensityEdit.Value)
            if (newIntensity < 1 || newIntensity > 100)
                throw Error("Intensity must be between 1 and 100")
            this.holdTime    := Integer(this.holdEdit.Value)
            this.lagStart    := Integer(this.lagStartEdit.Value)
            this.lagDuration := Integer(this.lagDurEdit.Value)
            this.intensity   := newIntensity
            this.CalcTimings()
            SaveAllConfig()
            this.statusBar.Value := "Saved — press " StrUpper(this.boostKey) " to boost"
        } catch as e {
            MsgBox "Error: " e.Message, "Invalid Input", 48
        }
    }

    ResetSettings(*) {
        this.UnregisterHotkey()
        this.boostKey    := this.defaultKey
        this.zKey        := "z"
        this.holdTime    := 50
        this.lagStart    := 150
        this.lagDuration := 400
        this.intensity   := 4
        this.enabled     := true

        this.SyncUIFromState()
        this.CalcTimings()
        this.RegisterHotkey()
        SaveAllConfig()
        this.statusBar.Value := "Reset — defaults restored"
    }

    ClearCounter(*) {
        this.boostCount   := 0
        this.sessionStart := A_TickCount
        this.countLabel.Value        := "0"
        this.sessionTimerLabel.Value := "session: 0m 0s"
    }

    UpdateSessionTimer() {
        elapsed := (A_TickCount - this.sessionStart) // 1000
        mins    := elapsed // 60
        secs    := Mod(elapsed, 60)
        this.sessionTimerLabel.Value := "session: " mins "m " secs "s"
    }

    PollRoblox() {
        global TintColor
        pid := ProcessExist(this.processName)
        if pid {
            this.rbxDotLabel.Opt("Background" TintColor)
            this.rbxTextLabel.Value := "Roblox Found"
            this.rbxTextLabel.SetFont("s8 Bold c" TintColor, "Segoe UI")
        } else {
            this.rbxDotLabel.Opt("BackgroundCC2222")
            this.rbxTextLabel.Value := "Not Found"
            this.rbxTextLabel.SetFont("s8 Bold cCC2222", "Segoe UI")
        }
    }

    ; ── REBIND (Boost Key / Boosted Key) ─────────────────────
    ; target: "trigger" rebinds the hotkey that starts the boost
    ;         "sendkey" rebinds the key actually sent (Z-Boost only —
    ;         Click Boost always sends a left click)
    StartRebind(target) {
        global TintColor
        if (this.rebinding || this.other.rebinding)
            return
        if (target = "sendkey" && this.useClick)
            return

        this.rebinding    := true
        this.rebindTarget := target

        box := (target = "trigger") ? this.keyBox : this.zKeyBox
        if (target = "trigger")
            this.UnregisterHotkey()

        box.SetFont("s9 Bold c" TintColor, "Consolas")
        box.Value := "..."
        this.statusBar.Value := "Press a key — Esc cancels"
        Sleep 200
        BoostModule.DrainKeyState()
        SetTimer(this.pollFn, 10)
    }

    PollForKey() {
        global TintColor
        if !this.rebinding {
            SetTimer(this.pollFn, 0)
            return
        }

        Loop 254 {
            vk := A_Index
            if !(DllCall("GetAsyncKeyState", "Int", vk, "Short") & 0x8001)
                continue
            if (vk = 0x1B) {
                SetTimer(this.pollFn, 0)
                if (this.rebindTarget = "trigger")
                    this.RegisterHotkey()
                this.FinishRebind(this.rebindTarget = "trigger" ? this.boostKey : this.zKey)
                return
            }
            if (vk = 0x5B || vk = 0x5C)
                return
            if (vk = 0x01 || vk = 0x02)  ; LButton / RButton stay reserved for clicking the UI
                continue
            keyName := GetKeyName(Format("vk{:X}", vk))
            if (keyName = "")
                return
            SetTimer(this.pollFn, 0)

            if (this.rebindTarget = "trigger") {
                ; can't steal the other module's trigger key — that's the
                ; one thing that has to stay unique between the two
                if (StrLower(keyName) = StrLower(this.other.boostKey)) {
                    this.rebinding := false
                    this.statusBar.Value := "Can't use " StrUpper(keyName) " — already bound to " this.other.displayName
                    this.keyBox.SetFont("s9 Bold c" TintColor, "Consolas")
                    this.keyBox.Value := StrUpper(this.boostKey)
                    this.RegisterHotkey()
                    return
                }
                this.boostKey := keyName
                this.RegisterHotkey()
            } else {
                this.zKey := keyName
            }
            SaveAllConfig()
            this.FinishRebind(keyName)
            return
        }
    }

    FinishRebind(keyName) {
        global TintColor
        this.rebinding := false
        box := (this.rebindTarget = "trigger") ? this.keyBox : this.zKeyBox
        box.SetFont("s9 Bold c" TintColor, "Consolas")
        box.Value := StrUpper(keyName)
        this.statusBar.Value := "Saved — press " StrUpper(this.boostKey) " to boost"
            . (this.useClick ? "" : " (uses " StrUpper(this.zKey) ")")
    }

    ; ── BOOST LOGIC ──────────────────────────────────────────
    TriggerBoost(*) {
        if (!this.enabled || this.boostActive)
            return

        pid := ProcessExist(this.processName)
        if !pid {
            this.statusBar.Value := "Error — Roblox not found"
            return
        }

        this.boostActive := true
        this.boostCount  += 1
        this.countLabel.Value := this.boostCount

        this.dotLabel.Opt("BackgroundCC2222")
        this.textLabel.Value := "Boosting..."
        this.textLabel.SetFont("s8 Bold cCC2222", "Segoe UI")
        this.statusBar.Value := "Boosting..."

        if (this.useClick) {
            Send "{LButton down}"
            SetTimer(() => Send("{LButton up}"), -this.holdTime)
        } else {
            Send "{" this.zKey " down}"
            SetTimer(() => Send("{" this.zKey " up}"), -this.holdTime)
        }

        if (this.lagStart > 0)
            BoostModule.PreciseSleep(this.lagStart)

        DllCall("winmm\timeBeginPeriod", "UInt", 1)
        this.throttleActive := true

        durTicks  := this.lagDuration * BoostModule.QPCFreq / 1000
        loopStart := 0
        DllCall("QueryPerformanceCounter", "Int64*", &loopStart)
        now := loopStart

        Loop {
            DllCall("QueryPerformanceCounter", "Int64*", &now)
            if (!this.throttleActive || (now - loopStart) >= durTicks)
                break
            BoostModule.SuspendProcess(pid)
            BoostModule.PreciseSleep(this.freezeTime)
            DllCall("QueryPerformanceCounter", "Int64*", &now)
            if (!this.throttleActive || (now - loopStart) >= durTicks) {
                BoostModule.ResumeProcess(pid)
                break
            }
            BoostModule.ResumeProcess(pid)
            BoostModule.PreciseSleep(this.runTime)
        }

        DllCall("winmm\timeEndPeriod", "UInt", 1)
        BoostModule.ResumeProcess(pid)
        Sleep 5
        BoostModule.ResumeProcess(pid)

        this.throttleActive := false
        this.boostActive    := false

        this.dotLabel.Opt("Background22CC55")
        this.textLabel.Value := "Boost Ready"
        this.textLabel.SetFont("s8 Bold c22CC55", "Segoe UI")
        this.statusBar.Value := "Ready — press " StrUpper(this.boostKey) " to boost"
    }
}

; ══════════════════════════════════════════════════════════════
;  MACRO FEATURE STATE
; ══════════════════════════════════════════════════════════════
global px          := 1038
global py          := 977
global targetColor := 0xFFFFFF

; Coordinate picker: true while actively hunting for a #FFFFFF pixel.
global g_PickingCoords := false

global rSpamEnabled      := false  ; toggled by checkbox; mirrored into config.ini for macro_engine.ahk

; Guard flag: true while LoadAllConfig() is populating the UI from disk.
; Setting a control's .Text/.Value during load fires its Change/Click
; callback, which would otherwise call SaveAllConfig() with only some
; fields loaded yet — silently wiping out whatever hadn't loaded first.
global g_LoadingConfig := false

; NOTE: the actual pixel-watch/burst state machine (keyGroups,
; currentGroupIndex, macroState, burstCount, burstKeyGroup,
; cooldownEndTime, burstSize, cooldownSeconds) now lives entirely in
; macro_engine.ahk — a separate hidden process — for speed. See
; LaunchMacroEngine() below. This script only owns the UI for those
; settings and writes them to config.ini.

global tabContentControls := Map(1, [], 2, [], 3, [], 4, [])

; Every real Edit control that can hold keyboard focus. DefocusTextBoxes()
; walks this list so clicking away from ANY of them (not just the Macros
; and Spammer boxes) drops focus back onto focusSink.
global g_FocusableEdits := []

; ══════════════════════════════════════════════════════════════
;  SPAMMER FEATURE STATE
; ══════════════════════════════════════════════════════════════
global spammerEnabled := false  ; toggled by the Spammer tab checkbox
global spammerKey     := ""     ; key(s) to press, from the Spammer tab textbox

; ══════════════════════════════════════════════════════════════
;  REBINDABLE HOTKEYS  (Hotkeys tab — Skill Spammer trigger + Cycle key)
; ══════════════════════════════════════════════════════════════
global spammerTriggerKey := "XButton2"  ; hold this to fire the skill spammer
global cycleGroupKey     := "g"         ; press this to cycle key groups
global g_HotkeyRebinding := ""          ; "" | "spammer" | "cycle"
global spammerTriggerBox := ""          ; set once the Hotkeys tab UI is built
global cycleKeyBox       := ""          ; set once the Hotkeys tab UI is built
global keyHint           := ""          ; set once the Macros tab UI is built ("cycle with X" hint)
global spamModeHint      := ""          ; set once the Spammer tab UI is built ("hold X" hint)

; Turns a raw AHK key name into something readable in the UI, e.g.
; "XButton2" -> "MOUSE5", "LButton" -> "Left Click", "g" -> "G".
FriendlyKeyName(key) {
    static names := Map(
        "LButton",  "Left Click",
        "RButton",  "Right Click",
        "MButton",  "Middle Click",
        "XButton1", "MB4",
        "XButton2", "MB5"
    )
    if names.Has(key)
        return names[key]
    return StrUpper(key)
}

; Refreshes the two static hint labels so they always reflect whatever
; keys are currently bound, instead of a hardcoded default.
UpdateHotkeyHints() {
    global keyHint, spamModeHint, cycleGroupKey, spammerTriggerKey
    if IsObject(keyHint)
        keyHint.Value := "Example: Z,XZ,C (cycle with " FriendlyKeyName(cycleGroupKey) ")`n"
    if IsObject(spamModeHint)
        spamModeHint.Value := "Spams a move (hold " FriendlyKeyName(spammerTriggerKey) ")"
}

; ══════════════════════════════════════════════════════════════
;  GLOBAL HOTKEY SUSPEND STATE (F3)
; ══════════════════════════════════════════════════════════════
global HotkeysSuspended := false

; ══════════════════════════════════════════════════════════════
;  BOOST FEATURES — Z-Boost (key) + Click Boost (mouse), independent
; ══════════════════════════════════════════════════════════════
global BoostProcessName := "RobloxPlayerBeta.exe"

; ══════════════════════════════════════════════════════════════
;  MACRO ENGINE  (native C++ replacement for the old macro_engine.ahk)
;  Lives in a dedicated AppData folder — config.ini, engine_status.txt,
;  and soru.exe (downloaded from GitHub) all sit together here instead
;  of next to the script.
;  >>> Fill in your GitHub username/repo below before using. <<<
; ══════════════════════════════════════════════════════════════
global RemoteBase        := "https://raw.githubusercontent.com/tavuk333333/soru/main/dist/"
global EngineDir          := "C:\Users\tavuk\AppData\Roaming\SoruMacro"   ; hardcoded per your request — NOTE: only correct on this machine/account; if you ever move to another PC or Windows username, swap this back to `A_AppData "\SoruMacro"` or update the literal path
global EnginePath         := EngineDir "\soru.exe"
global EngineVersionPath  := EngineDir "\version.txt"
global ConfigFile         := EngineDir "\config.ini"
global StatusFile         := EngineDir "\engine_status.txt"   ; matches what engine.exe derives from ConfigFile's directory — kept as its own constant so PollGroupStatus doesn't depend on string-parsing ConfigFile

; Created immediately (not just inside EnsureMacroEngine) because ConfigFile
; also lives in this folder now, and SaveAllConfig() writes config.ini
; before EnsureMacroEngine() ever runs — IniWrite creates the file but not
; missing parent directories, so this has to happen first.
if !DirExist(EngineDir)
    DirCreate(EngineDir)
; Explicitly un-hidden — AppData itself is a hidden folder in Explorer,
; but that's an attribute on AppData/Roaming, not something that gets
; inherited by subfolders we create. This just makes sure DirCreate (or
; any future code) never leaves SoruMacro itself flagged hidden, so once
; you've typed the path into Explorer once you can find it again without
; "show hidden items" turned on.
try FileSetAttrib "-H", EngineDir

; One-time migration: earlier versions kept config.ini next to the script.
; If that old file is still there and nothing's been written to the new
; AppData location yet, carry it over instead of starting fresh.
global OldConfigFile := A_ScriptDir "\config.ini"
if (FileExist(OldConfigFile) && !FileExist(ConfigFile)) {
    try FileCopy(OldConfigFile, ConfigFile, 0)
}
global g_MacroEnginePID   := 0

global zBoost     := BoostModule("ZBoost",     "Z-Boost",     "b", false, BoostProcessName)
global clickBoost := BoostModule("ClickBoost", "Click Boost", "n", true,  BoostProcessName)
zBoost.other     := clickBoost
clickBoost.other := zBoost

DllCall("SetPriorityClass", "Ptr", DllCall("GetCurrentProcess", "Ptr"), "UInt", 0x0080)
BoostModule.Init()

; ══════════════════════════════════════════════════════════════
;  EMBEDDED LOGO IMAGE (base64) — no external file needed
; ══════════════════════════════════════════════════════════════
global LogoBase64 := "
(LTrim Join
/9j/4AAQSkZJRgABAQAAAQABAAD/4gHYSUNDX1BST0ZJTEUAAQEAAAHIAAAAAAQwAABtbnRyUkdCIFhZWiAH4AABAAEAAAAAAABhY3NwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAA9tYAAQAAAADTLQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAlkZXNjAAAA8AAAACRyWFlaAAABFAAAABRnWFlaAAABKAAAABRiWFlaAAABPAAAABR3dHB0AAABUAAAABRyVFJDAAABZAAAAChnVFJDAAABZAAAAChiVFJDAAABZAAAAChjcHJ0AAABjAAAADxtbHVjAAAAAAAAAAEAAAAMZW5VUwAAAAgAAAAcAHMAUgBHAEJYWVogAAAAAAAAb6IAADj1AAADkFhZWiAAAAAAAABimQAAt4UAABjaWFlaIAAAAAAAACSgAAAPhAAAts9YWVogAAAAAAAA9tYAAQAAAADTLXBhcmEAAAAAAAQAAAACZmYAAPKnAAANWQAAE9AAAApbAAAAAAAAAABtbHVjAAAAAAAAAAEAAAAMZW5VUwAAACAAAAAcAEcAbwBvAGcAbABlACAASQBuAGMALgAgADIAMAAxADb/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAF9AgwDASIAAhEBAxEB/8QAHgABAAEEAwEBAAAAAAAAAAAAAAgEBQYJAQMHAgr/xABPEAACAQIEAQcKAwMICAQHAAAAAQIDBAUGBxEhCBIxQVFX0xMXGBlWlJWW0dIJImEUUnEVIzJCYoGhsRYkcnORkqKyJTPC1Edks7TBw/D/xAAdAQEAAgIDAQEAAAAAAAAAAAAABwgFBgIDBAEJ/8QAPhEAAQMCAgYIBQMCBgIDAAAAAQACAwQFBhEHEiFRU5EUFRYxQVJh0SJUkqGiE3GxgeEjMnKCwfBCQ2Li8f/aAAwDAQACEQMRAD8AjofUJuD3TOGmjggxfqksiy/m68wS4jUo1pR2e/BkntHeVPd4PWoUry4k4x2XF9JEBPY76F3OhNSjJprsPdT1ktM7NhWr3nDtDeojHUMB9VuMyHrVg2bbWi/2mCnNLrR6NSr068VKnNTi+tM01ZN1axXK9eEqNzOKX6kp9LOWFWpqjQvaz3Wy3kzeaO+xygNl2FVWxJorrKIumt/xN3KeQPO8j6xYVm63pyhXhzpLtPQoVI1YKUGpRfQ0bQyRsg1mHNQVVUc9FIYp2lpC+gAdi8aAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiHDaS3b2R8Vq9O3pudSShFcW2eK6va+YblOzr0qNwnOMWuD47nRNMyBus8rKW621NzmEFM0klZHqjqzh+TbGqncQVSMXvs+Jr51t13vMz39aFK4l5Jt8Nywaua1X2bMQrbV5eTbfDc8aurudxUcpSbbZHdyurqgljO5XKwRgCGzxtqaoZyH7Lsv8QqXlWU5ycm+0o29wEtzWCc1ObWhoyCJbndC3lJbqO5ccFwKvitxCnSpuTb24I9/wAo8nO/xTBKVw6EvzN9KZ6oaaSc/AFgbneqS1tBqHgZrzLO+kOK5XuakattNRi2t+aee3NnUt5uM4uLRuQz1ozg2baFTnW8VUkujYhnrPyVLjCa1atZUG48Wkl1GfrrLJBm6PaFEmFtJ1HdNWCs+B6ho1scGT5jyZe4FcTp1qMouL24oxudJwezRrDmlpyKnSGeOdofGcwV8ptFTbX9S2knCTi12MpQce5dpAcMivU8hayYnla4puFxPmp8Vv0kxtIeV3b3NClbX9Rb8F+ZmuhSaK6xxevYzUqdSUWuxmWpLlNSn4TsUfYgwVbb8w/qsydvC3Q5X1JwnM9CE6FeKlJdG5lcZKcVKL3T60ah8ga8Yvli4p7XM3FdKbJj6N8qmjjEaVteVlvL96XWbxR3qKoya/YVVrEujSvtGtNTfGwKWYLVgOYrTH7WNW3qRk2t9ky6mxghwzChiSN0Tix4yIQAH1daAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiFBiuM2uD2861xUUIxW/SWfN+fMPynZVa1erHnRXQ2Qo105UFS9lXtbKsknut0zG1ldFSNzcdq3bDuFa7EEwbE0hniV6brjym7bC6Ve2sqq53QtmQWz7qZfZovqs6lebi23tuY9mPNV1jd1OpVqylu9+LMelNye+5G1bcZKt207FdbC+DaLD8I1W5v8SvutWlVk23uzqB9Qg5NbLcxCkbYAkYuTMgy5li5xq6p06VKUuc+pFblDJd3mC9p06VKUt31InLoDybadtSoXd7Q3lwfFdBlaKgkq35AbFoOJ8V0mH6cvkd8XgFjfJ+5N3l1Qu72g9uDSaJnYLkzDsIw6lawoR2guwuOEYNbYNaQoW9OMIxW3BFeSZSUUdKzVA2qjuIcT1l+qTLI4hvgFrk9bJifd3a/EpfYUeJfinXWLUHSuNNrOcX24jLwyBwLSHCNlIyMH5O91Ajb3XsOs2TI/sFJLO3K1wrPM5yq5Eo2U5f1oXzl/6DzC8zbYYrUcqVj+zb9Xledt/geeH3CtKm90zT7vo0s9c0mmZqO/c/8AJUwYX0v3uxubHUO/UjWb+XhU/orb+8GL2mKyptKTL3bYhCqlx4ldsQYGuFlcXauszeFdDCOlG0YmYG6+pJuKrQcRmpLgckauY5hycFNDJGyDWacwvqM3F8C7YRmm9wWtGpQm01+uxZwGuLTmF8kiZK0teMwpBZF5a2aMgRgo4ZSxCMf6s67jv/gz0NfirYzawUauntrUa/rLEZL/ANBDlrcpbmxhVT4cSTcN4goaZ4iuMOu3fmR/BVdMc6MetWuqbY7Uk3ZDapnetkxPu7tfiUvsHrZMT7u7X4lL7CC93hLhu4otlSlKm9mizFrt2GLvGJKaMH01ne6o9fbfiHD0xirWkZeOQy/hT79bJifd3a/EpfYPWyYn3d2vxKX2EAgZ/slZeB+Tvdal1zW+f7BT99bJifd3a/EpfYPWyYn3d2vxKX2EAgOyVl4H5O9065rfP9gp++tkxPu7tfiUvsHrZMT7u7X4lL7CAQHZKy8D8ne6dc1vn+wU/fWyYn3d2vxKX2D1smJ93dr8Sl9hAIDslZeB+TvdOua3z/YKfvrZMT7u7X4lL7B62TE+7u1+JS+wgEB2SsvA/J3unXNb5/sFP31smJ93dr8Sl9g9bJifd3a/EpfYQCA7JWXgfk73Trmt8/2Cn762TE+7u1+JS+wetkxPu7tfiUvsIBAdkrLwPyd7p1zW+f7BT99bJifd3a/EpfYPWyYn3d2vxKX2EAgOyVl4H5O9065rfP8AYKfvrZMT7u7X4lL7B62TE+7u1+JS+wgEB2SsvA/J3unXNb5/sFP31smJ93dr8Sl9g9bJifd3a/EpfYQCA7JWXgfk73Trmt8/2Cn762TE+7u1+JS+wetkxPu7tfiUvsIBAdkrLwPyd7p1zW+f7BT99bJifd3a/EpfYPWyYn3d2vxKX2EAgOyVl4H5O9065rfP9gp++tkxPu7tfiUvsHrZMT7u7X4lL7CAQHZKy8D8ne6dc1vn+wU/fWyYn3d2vxKX2D1smJ93dr8Sl9hAIDslZeB+TvdOua3z/YKfvrZMT7u7X4lL7B62TE+7u1+JS+wgEB2SsvA/J3unXNb5/sFP31smJ93dr8Sl9g9bJifd3a/EpfYQCA7JWXgfk73Trmt8/wBgp++tkxPu7tfiUvsHrZMT7u7X4lL7CAQHZKy8D8ne6dc1vn+wU/fWyYn3d2vxKX2D1smJ93dr8Sl9hAIDslZeB+TvdOua3z/YKfvrZMT7u7X4lL7B62TE+7u1+JS+wgEB2SsvA/J3unXNb5/sFP31smJ93dr8Sl9g9bJifd3a/EpfYQCPqFOVR7RRwfhWxxtLnwgAf/J3uu2O6XCZ4ZG7MncB7Kfa/FjxN/8Aw6tfiUvsOu8/FGzBitCVK3yNaWjktud+3yk1/wBBBywwpyackXy3tY0orgQjiy74ftoNPQQgv36zvdWewBo4u15c2suh1Y92Q2r2zPHKnzNn1TVehC0hLpjCq2eUXuLV7+o51ZNt/qUSWwK7VNS6peXFXetdnpbTCIadoGS5b3OAcOSijyNaXHILNucGDN3cvrfbizsoYxQw+op1KHlUv6vO2LZeX8aUXx4mP3l/Ks2kyWMK4Fqb28SSjVjUBY90o0OGonQwO1pdwXv+Q+VTh2ntSE4ZLpYhUg9+dO9cd/8AoZ7VY/iq3mH0IUaGm9pCEVsksSl9hArpBZ2iwPZKOIM/RzO/N3uqBXrGl2vdSZ55O/uGxT99bJifd3a/EpfYPWyYn3d2vxKX2EAgZDslZeB+Tvda71zW+f7BAAbesKgACIdtK4nSfBnUDomgjqGFkrcwvTT1U1JIJYHFrh4hXi1xhxaUi7W9/CqukxE7aNzOjJNNkRYg0cUFyaZKUaj1YbCOmW62Z7Ya8/qR/dZopKRyWWxxRSSUmXenVVRbplV71h6sssxjnbs3q92GsX27EtO2aleM93ivsAGrdy3rvXzOmpLiigusNjUT2RcQZu3Xirtkgkp3kLWbxh233uExVcYdn6LE7rDZUm9lwKFxcXs1sZrVt41E90We9wnfdxRZnC2kmKr1ae4bHb1SXHeheegLqy0DWb5VYgdta3lRk00dRPUM8dQwSROzBVUamlmpJDFO0tcPAoADvXmQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQDbfoK2zw+VZptGLuFyprZCZqh2QCzlos1ZeqhtNRsLiV029rKvJcOBfbHDFBJyRU2ljGlFcCsSSKpYu0gz3FzqejOqxX30e6I6WzsZWXFutJ37fBfNOmoLgj7AIPkkdK7WccyrPxQshaGRjIBAG9jor3UaS4s7qellqniOJuZK81ZWwUMRlncGgb12zqKC4stV/iagmk+JSX2K77qLLTUqOq92WNwho5Li2ruIyG5U30iaZGsD6CznM9xcuy4uZV5Pd8DpALK09NFSxiKFuQCpXWVk9fMZ6hxc470AB6V4kAARAAEQABEAARAAEXMJuD3TLnZYo4bKTLWDBXWy0d3iMVSwH1W02LEtxw9OJ6KQjLw8Fl9tfQqxXEqlJMwyjdTovgy7WeL77KTKyYl0aVNGXT0PxN3K7mCtNdFcQ2lunwP3+CvwOijdRqrgzvT3IMqKWaleY5W5EK0dHXU9dGJad4cDuQ4lFS6TkHna4sOYXtexrxquGYVvvMPjVT2RYbyxlRk2lwMua3Ke4tY1Y7NEu4Ux5VWZ4inOtGq9490VUOI4nT0zQyXeFhrWwLvfYXzd3FFqqU3TezRbOz36jvUIkp3bdy/P/EWFblhqoMNYwgb/AAXyADY1pyAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiABcXsj4TltK+gEnIIfVOlKq9kiptbCdaS3XAvtphsaaTaI2xJjahsbC1rtZ+4KaMGaMrpieRr3tLIt5VBZYTvs5IvVC2jSikkdsaaitkj6KmX/ABXXXyUmV3w7l+gGE8BWvC8DWwRjX8SnQADR+8qTtgCHEpKKPipWVNbt7FovsWUd1Fm12TDlbepgyBmzetDxNjG24ZpzLVSAHcqy7xCNJPjxLBeX860mk+B0V7mdaTbZ1FuML4HpLIwSSjWkX58460oXDE8joadxZD/KdPSACUQMtgUFkknMoAD6viAAIgACIDLPM9qH3dZz+W77wh5ntQ+7rOfy3feEefpEPnHML1dFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYTaMs8z2ofd1nP5bvvCHme1D7us5/Ld94R8NRAdheOYX0UtSDmI3cisetr6dGS4vYvdnicaqSbO7zPah93Wc/lu+8I+6ekmotN7rTvOfy3feER3iLClqvkZcC1r9+YUw4O0gX3C8rWkOfFuIKqYVFNbo+iottO8/Wq3uMg5toQXTKrgF5BL+90jmrg2K2fC7wnELR9auLSpTf/VFFTL/hmpsspa7a3eFf/CeOKHE1OHsOq/xB2KmBy4Sj0xaf6o4NK7lJnevmdNTXFFqvsMU4tpF3OHHnLY2WzX6rs8wlgcRktLxHhW34jpnQVTASfFYZXtpUW91wOky25wuVynzKU5t9UYt/5FFHImYr2X+o5cxq+36P2XDa9Xf/AJYMt5hfHNHeowydwbJ6lfnnjnRdccNzOlpmF8XoM8lj4MrjpDqDNbx08zjJdscuXrX/ANI58z2ofd1nP5bvvCJL6TD5xzChLolQP/W7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94R96RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMs8z2ofd1nP5bvvCHme1D7us5/Ld94Q6RD5xzCdFqOG7kViYMsekGoUVu9O85Jdry5e+EUlbTvNVk/9dyrj9iv/AJnCbml/3QR5qi40lLGZJZQAPUL20dor6+UQ08LnOPoVYIQdSWyRdbHC22nJFwtcCq2zXlberTfZUpyj/mi4QpKmtttiuuLtI+sHUttOzerjaPdDWoW115bt7w1ddC1jSS4HelsAVwqaqWqeZJXZkq5lHQwUMQigaAAgOUtz7p21es9qVvVqvsp03J/4I8zWlxyC9j3tYNZx2Lrb2Ka4uo0lxZdllDNN7H/Usq4/eb9DtsKuKu//ACwZQXWl2oldvm6eZya7Vly9f/6iV8LYKlu0gkqCGs9SoCx1pNprBE6GkBfJ6DNY3fYm5tqLLZKTm929zK/M/qH3dZz+W77wh5ntQ+7rOfy3feEW2tNut1nhENOWj1zC/Pu/3q74iqTUVgcc/DI5BYmDLPM9qH3dZz+W77wh5ntQ+7rOfy3feEZ7pEPnHMLVei1HDdyKxMGWeZ7UPu6zn8t33hDzPah93Wc/lu+8IdIh845hOi1HDdyKxMGWeZ7UPu6zn8t33hDzPah93Wc/lu+8IdIh845hOi1HDdyKxMGWeZ7UPu6zn8t33hDzPah93Wc/lu+8IdIh845hOi1HDdyKxMGWeZ7UPu6zn8t33hDzPah93Wc/lu+8IdIh845hOi1HDdyK32AAqIpbQABEAARAAEQABEAARAAEXVcW9O6pSp1YKcJLZpnh+rnJ9sM2WdadC3h5VptNLie6g6JoGTt1XhZW3XOqtcwmpnZELU5qroBiWVritOFvN0k3xS6DxG+wyrZVZQnFxafWbpc5ae4dmyzq06tGHPknx2IR65cl+vhtS4ubK33im3tFdRoVxsrovji2hWzwbpMhuGVLXnVfvUKmtjgyDMOV7nBbqdKrTlFxe3FFhlFxZqTmlpyKsLFKyZoew5gr7o15Uppp7M9EyBqjf5Xvac6deUYprhuebH1Gbi99znHI6M6zSvPV0cNbGYpm5grZpofyl7TF7ehbXldKfBPnMk5hmLW2LW8K1tVjUjJb8GaUst5vvMCuYVKNaUXF9TJcaJcqmth6oWt7W3S2W8mbzbr0CBHMqrYz0ZPjLqu2DZ4hbAQYbkrUvDM32dOpRrR58lx2ZmKe63XFG5Me141mlVsqKaWlkMUzciFyADmvMgACIAAiA8x1s5QuUtBHllZnuvIPHsQjY0VFr+bjw8pXn2U4c6HOfVzkempppNPdPrO98EscbZXtIa7PI+By2HL9lwD2ucWg7R3rkAHQuaAAIgACIAAiFPe31Gwoyq1pqEV2ssebc62OVrSVSvWhGaW+zZDXXLlTTnUrW1jcbpbpOLMdV10VI3Nx2rcsP4XrsQTBkDfh3r2fWrlGWWWLSrb21ePP2fQ+JA3VDWzEc3XlVyuJcxt7JMwrNufL7Md1UqV60p8578WYlUqub3b3ZHdfdJKpxAOQVysKYEorDEHObrSb13XV5O5m5Sk22U++5wcpbmCUqgBoyC4OylRlVkkk2yqsMMq3tRRpwcm+xHu+kHJ7xDNt3SlUoSjTbTba6j0Q08k7tVgWHud2pbVCZqh4AC8yybpziGZrunToUJz5z6kTF0S5KTU6Nzf0OjaT5yPctKOT1huUralVrUYOolw4HtNraUrOkqdKChFdSN8t9kbEA+bvVTcXaTp64uprccm71YsrZHw7LFtCFC3gppbbpGRgG2NaGDJoVfpppJ3mSU5koADkulAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQt2MYHa43bypXFNSUltu0XEHwgEZFc2PdG4OaciFETXPkwUcQhXubGlHfZyWyIN5704vsr31WnVoyiot9RuauLandU3CrFTi+pnh+sPJ+w7NdtVrUKKU5Jvgus1S42dswL4htU/YM0kTW5zaWvObN61NVaMqcmmtmdZ7hqzohf5Svq21CTpp9KieM3llUtajjOLi12mgTQvhcWvCt5b7nT3KETQOzBVMnsVlliNWzqRlCbi0+plG1scHQDkso5ocMivftJNfMQypdUoTuJeS32abJ7aQ682GcbClTq1ourslvuakKNeVKSaezR6Hp/qjiGVLylOlXnFRa6GbFb7tJTENccwocxfgCkvcbpYW6sm9bkqFeFxTjUpyUovimjsIn6F8py3xajStL24jvsl+ZkoMHxy0xq2jVtq0KifSkyRKeqjqW6zCqaXmxVlknMNSwjLxVwAB7FriHVd3dGwta1zc1YULejCVSpVqS5sYRS3cm+pJLc7SE/4lPKHWSMlUNN8GuXDHMw0nVxCdKWzt7FS25r7HVkpRX9mM+1b5S2W+S51bKWLvcdp3DxP9AvNU1DaaJ0r+4KD/Ku14q8ofV3Esfpzm8Atk7LCKFTdKNtFv8/NfQ6j3m+vZxT6DYT+HnyhfO5pY8r4tcyq5nyrCnb1JVZbzubRpqjV3fS1zXCXXvFN/wBJb6lj0Xk/az4hoJqpg+brJVa1vbz8lf2lJ7O5tZcKkOPDfbaS/tRRP95sEVZaxRU7cjGPg/ceH+7x9dqj6iuL4qszSHY7v/76LemC3ZdzDh+bMAw7G8JuoXuF4jb07q1uab/LUpzipRkv4pouJW1zS0lrhkQpJBz2hAAfF9QAocVxm1we3nWuaihGK36T4SAMyuTWOeQ1ozKqq1eFCDlUkoxXWzyvUvXPCcoWtWMbiLqRXTujyfW/lOW2FU69vZVU5cUtmQbz9qpiGaLyrKdeThJt7bmsXC8sgBZFtKnXCGjaourm1FcNVm7evTtZuUXe5nu69OhXkqbbS2l1EecTxetiFaU6k3Jt9ZR17mdablJ7s6iP56mSodrPKt7arLSWiEQ07AMkb3OAd9C1nXkoxTbZ5VniQBmV1RpuT2SMjy5lC8xy4hTo0ZTcntwRmGnOkOIZrvKUYUJuDfF7E7dE+TPZYFZ0bi9t06mye8lxM1Q2yWrd3bFGOKcb0OH4iNbN+5eKaGcmCpf1KNzfUHzN09mukm1kjT3Dso2FOnRoQVRLi9jIMMwe1wm3hRtqUYRituCK0kWjoIqRuTRtVNMR4trsQTF0riGblwlsjkAya0ZAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQ4lFSWzW6OQEWAaiaVYZnGyqc+3h5Vrjw6SDGtvJnusFuK1azoSdPi1sjZMWrHMuWWPWs6NzRjPdcHsYitt0VW3aMipDwzjOuw9KNVxczctKON5busJryp1aUoNPbiizSg4s2Q66cl+3xChWu7C32fFvmIg9nvTO/wArXlWnVoSiovsI6rbdLSO2jYrmYZxlQ4ghBY7J25een1Gbj0H3Voypyaa2Z1GJUh96v+AZpu8FuIVKNWUOa9+DJQ6K8qK5wevRoXtxJ0+CbbIgJ7FRa3lS2mpQk4tHtpquWmdrMK1e9Ydor3CYqhgOfityWn+r+E5wtYc24gqjS/rI9BhONSKlFqSfWjUDpprLiOVb2i1cS5kX0Nk7tFuUjY5ho0aF1VSk0k031kg2+7x1IDX7Cqf4u0d1Vlc6elBdGvcNQ8+YTpjknGc1Y5X8hheFW0rmtJcZS2XCEV1yk9opdbaNGuq2pmL6w6g41m7G5t3uJV3UVHnc6NCmuFOlH9Ix2X68X1kvPxKeUes041Y6Z5fuufhVgo3mMVKb3VW5fGnR/hBfmf6zj+6yCzaSbfBIs/gq0CjpOmyj45O70b4c+/8AbJVZvtWZJujDub3/AL/2VywbLuKZi/bf5MsK9/8AsVtO8ufIQcvI0Ibc6pLsit1xLcbYfw/+TZQ010jusfzDYRqY/nChGVxQuI7+Ssdn5Og4vo5ylKcu3nRT35qNfvKp0Er8nnVzEcvU4VHgNzveYPXqNycraTe0HJ9MoP8AI30vZN9JnbfiGnuFwmoWf+HcfNl/m5Hu3jasfU22SnpmTnx7xu3KW/4ZHKHd7aXek+OXLde2jK8wKpUlvzqW+9a3/jFvnxXY5fumwI0AZNzhi2n+a8KzJgdz+yYvhlxG5tqu26Ul1NdcWt011ps3j6K6rYXrXplgWcMKajSxChGVa35ylK2rpbVaMtuuMt1+vB9ZGWNbP0SpFdEPgk7/AEd/fv8A3zWz2St/Xi/Refib/H9lnBw2ordvZdrKPE8YtcJt51rmrGEYrfiyO2snKascv2lahZ3EXU2a2TIqnqY6dus8qQrVZay8TCGlYTn4r13POqmGZPs6tSdaDnFPrIUa1cqi5xSVe2s6/wCV7rdM8W1K1wxPNdzVTuJ+Tbey3PJLq/qXU5SnJyb7TQq+9PmzZFsCtphHRnTW0NqK4az1d8w5qusbuZ1K1WU93vxZYJTcnvucN7nBqrnFxzKnyKJkLQxgyCHKi2z7p0pVJJJbmX5QyDfZivKdOlRlJSaXBH1jHPOTVwqKiKmYZJTkArDhOB18Trxp06bk29uCJHaPcmi/zDWo1ri3kqba4OJ7BoZyW1B0Lm+orjs2mugmDl3Kljlu0p0balGPNW26RuduspdlJMq1Yy0nNg1qS2nM71g2meiuHZMsaKlRh5SKW/5UeowhGnFRilGK4JI+gbxHEyJuqwZKrNbXVFfKZqh2ZKAA7V4EAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARddehC5pSp1IqUJcGmeDa86KWGLZaxHEqdlUuXbUZVpUrampVZKK3aiut8Og99OGt1s+KOmWGOYasgzCylvudTa5hPTOyIWkfMuNZXxC6qSwq4ryg3wVWg4NGOSlCT/K91/A9r5eugXmR1ZljuEUJU8r5oqVbuhGMfyW1zvzq1FPoSblz4rsbS4RI82WKqSSkzpvWA3Npm3C15vicM/Ubwf2OxWdwXpcp61/V92IZKNmfgfVXkHXTrRqLdM7CH5YXwuLXjIqzMFRFUsD4nZgr7hUcHunsZLl3Pt/lmflbSpJVYreHHhv1bmLg4MeWO1gk9PHUsMcgzBVnx2jcYne3N7c1Z3F1cTlVq1ZveU5N7tsvGjTybh+pmC3moTuZZUs637TdW1rQdaVy4cYUmt1+WUtud+ia6z5nBSRbrzDI1E2kWHwrpCLYugXBxDSMsx3jw2blT/H+h1s8huNoaNYbS3wK2fr8TPRmKSUsdSXBJYa+H/UeA8srlPaMco7Tana4c8Wo5twmr+0YVdV8PcY/m2VSjKXO4RnFL+EoxfUQhubKVGT4cCmjFyeyXEmW1YfszSy4Ucjvh2g6w++zuPcVUS5yXSlldQ1cQDjsyyP2XBJ/kW8q6tye7vHMJxmFxd5WxOn5enTornztruOyU4rf+jOPCX6xi+0jjZYXKq05IvtrYxoroNYxpjK2wUz6EfGT9jvClvR/osul3nZW1I/Tj/kKX2qPLYp5yhUp4VC7pUpb/APmw5v8A+SOGYM53mP151K1WcnJ7/mZjaio9ByVKrK11U8u8FfqxYaorHAI4GDPevudRzbbZ8AGMW3dyFTY2k72sqdNOUv0RSOTc4winKcmoxiult9CRLzk/8l2+xGFtc4lbSUp7TqKS6G+oyVPQTT5ZDvWqXrEdDZYnSTvGY8F5xpPoTiGbb2i5UJKnJri0Tv0l5OGGZXt6NW4oxcopPZri2eiZA0xw3J9nBU7eCqJJLh0GbpJLZLZG/wBBaY6YBzxmVTjFmkCtvcjoqdxbH/K6LOwoWFJU6MFCK7EVABsGWSiAkuOZQAH1fEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABF5jyjtFLDX3SbGMqXfMp3dSP7Rh91KO7trqCfk5r9OLi+2MpLrNIeM4TfZZxvEMJxGhK0xLD7mpaXNCa2lTq05uE4v+Ek0foKNbv4nHJ9jg+KWmq+D2yha306djjUaceEa23No15f7SSpt9qh2kn4KuwhmNtn/wAkn+XPwdu/3fyBvWtXmlc5gqodjm7t39lDfT29wqvmnD7XHm4YXcT8hVqqbj5Fy4RqbrqT23/Tc9f1A0Su8nXE1ClOVH+rLdtNdT3I4tbrY2Fck7POCa06I4ngeabijTxvJ1o3Wu7ifGtYRTcK0m/3Ipwb/spvizG6QsFRyxC40LciNjgPXuPPYf6KYtG2lSrtUgoLg7WZ4EqG11bStqjhNbPsZ0nfmTM9pj+ZcQurKLp2E60lbRfT5NP8rf6tcf7ynhLym23HcrTcbTVWuT9OpYQVe+yYgob7AJ6SQOBXJ9Lmt/mW5w1szgwwJHctmIBGRV2wu1wCrUj/AClYSuIdajXqQ3/5Wi8XuGafwob2mBVadb9539eX+DmYlu0N32mcgvlypojDFO4NPgHHJalV4Us9dOKmeBpePHILvuaVlCTVtQdKPVvOUv8ANlOAYeSV8p1nnM+q2aGCOnYGRNyAQ56TgumAYW8YuJ0oNb04uc9+qK6X/BHxrHPOTQuUszIW60hyCtslzIbvr6Ciub2NFPidON41Tlc1PIv+aX5Yfqu3+/pOrJuWMV1MzrguV8HpOtieL3ULS3jtvs5dMn/ZjFSk31KLfUTXhfR9PXAVVd8EY2nPcqz450v0do1qO2nXl7tm9Sl5BnJ/nq/nC5zhilHnZdwCsqVJSj+Wvec1SS/XmRlGT/WUTZ9g+B2mC20aVtRjTS6WkY9o/pfhWjenGCZQweCVph1BQlV5u0q9V8alWX9qUm2/4mZHtqhT/rHozcmDY39h4/ue9Vhr7xXXV36lY8knadyAA8yxCAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIWHPeS8L1FydjGWcaoK4wvFLadtXpv92S6V+qezT6mkX4HNj3RuD2HIjaF8IBGRWhfV/THE9G9Sseydi0ZftWGXDhCs47K4ovjSqx/SUGn+j3XSmYxY4peYZG7jZ3Va1V3Qla3CozcfK0ZOLlTlt0xbjHddD2Nmn4nei1lmDTW11It5UbbFsv1Kdrc89qLubarVUFFPfjKFSaaXY57GsIs7Ybm28UDKhw+Lud/qHvsP9VF9wpTRVBY3u7x+3/di7KNeVJ8GSW0x5P8AieJaUwznicHb0sRbdhTmuMqC4eV/hJ77fok+s8V0lyVb58z/AIRhN/cQtMNqVPK3dSctm6MFzpxj/aklzV2b79CZL3WPX+lcWEMFwuFO3w+1pqhRo00lGEIraMUuxIhrShcLa2NtJkDKdpO4eA/qf49VZDQ7QYhdVdIpiRD3be71UYsewv8Ak28nS64totJccWxGWIXE6knu29y3FU3ZZ7F+hsIeIxr96AA4rvQ5RwAi6rmbpJljucZubWdTyNWdLykJUp8xtc6Els4vtTXUZFVSrU0muK6y13mGRqJtLiSpgm5WqiqR1hHn6qCdJllv1zoj1TLq5eA8VjM5ym929zY9+GNyfv5Mwa61Wxi3cbrEIzssGjUjs426e1Wsv9uScU+yLfRIhJotpZDVPVvLWVLq7hh9liN1tc3E3tzaMIyqVFHtk4waX6tG8PL+GYfgmCWOG4VSp2+HWdCFvb0aSSjCnFJRSS/RFgsU4hp3ULKOgcP8QZnLwbu/qfsPVUAprJW0dW+S4tIc0+Pid6uIAIcWxIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAeK8rLX230C0lxHFaE41Mw3n+pYTb9LlXkn+dr92C3m/8AZ26WjuhidPI2JneTkvoBdsaM1Cb8SjlDLPGd6Om+C3Mp4Pl6r5TEpU5fkr3239Dh0qkm0/7Tkv6pDazsJVpbtcC5Rt7jE7yve3lWdxd3FSVatWqveVScnvKTfW223/eXa3tI048OGxuV4xrSYdohbLYc3NG128+J/qVJ+DNE1Zf6gXK7DVjO0N9PBdWF0JYdXpV6MnTrU3zoyjwaZdrvEKl3NznJtvpKRLYFabjcZ7nMZqh2ZKvLaLLSWWnbT0rA0Bc77nByluV+HYXWv6sYU4OTZiwCe5ZtzgwZuKooUZTfBbna7GslvzHt/AkLo9ydr3Nt1RdehKNKWzb2JLXvIyw9YFJwpp1lHgtuJmoLTUVDNdoUaXbSBaLTUCnlfmTuWt6VOUXs0fJ73qlyfcSyrcVZU7ebpJvikeI4jh1TD6jjVi4PfbiY2aCSF2q8LdrddaW5xCWneCCqINbgHnBIOxZcgEZFfdjc3GE4la4hZVZW97a1Y1qNaD2cJxe6aJ8aH8sOV/ZWkMTqqFXmqNWMn/W6/wD+/UgIVeHYjWw6tz6U3H+DM3BdqiEButmAtAvuDLZe2H9WMBx8VumyrqPhOaKFOVG5p86aWy3MsjJSW6aa7UaitO9d8VyxcUk7ibpxa4Nk1dIOVFZY3Qo0byuvKbJPnM3SivEVR8L9hVU8TaN6+zEy0w12KUILdhOO2eNW8K1tWjNSW+yZcTYgQRmFDb2OjcWvGRQAH1cEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEB11q8KEHKpJRiutnlWpuueE5QtK0Y3EXUjF8d10nVJKyJus85LIUVBUXCUQ07C4lZNn/USxydYVJ1a8ISjFt7yXA1P8pPWO81w1FrXs605YPh7lb2NJt81rf8ANU27ZNcP0SMz5QGvl/nOrcWdtcSjTrtxk0+Kj1ngEYqK4GmVd9czW/QOR7vdW2wXo3io2tqa9ubu/JcQpqC4I+9+BwDSJJHSO1nHMqw8UTIWhjBkAgDexT3F1GlF8T0U1LLVyCOJuZK8tbXQUERmncAAslytl6tmK58lbxdSSaTS4kvdBuTJVv6tG5vrd8xbPiiE2T9VsW03x7+VcGjaVbjmODpX1J1aUl2uKlF7rq4ns+F/iQaw4Nbxo2lvlKlTXBf+E1d//uCbbRozuczWzvDQD4E5f8KoGN9MlKS+ht7/AEJC2nZNyJh+U7KFOhQhGe3TsZPtw26jUv6znWz93Kfwmr/7ges51s/dyn8Jq/8AuDeW4EujBqt1Of8AZVlnxFTVEhkleST6FbP835Aw3NNpVhXowTknu2jTtrrnLBsX1Rxm2wBqeC2FxO0oV49Fdwk1Oou2Laez60k+szbNH4i+s2bcu4jgtzc4BZW9/Qnb1LjDsOqUriEZLZuE3Wkovbr2ZGSL5m23DYylHo7ZOyQ3LLWOxuRzy3k7OX9VmLbpGrbDKw295LfEH+Fm1Csq/NUeLfBJHfUpSpvZrZmOZfzJVwO+hcwo0LipBNRjcxcoLdbb7JriuriX2WZ54pLnVKdvCT6qcGl/i2QdiXAlfZ5C+JutHvCuvgvSrasRxNjmeGS7ivsHCqqfHh/cckXPY5hycMlOscjJW6zDmF9Rm4vdMv2A5qu8FuIVKNaUXF78GY+c77HFri05hfJYmTNLHjMKYGi/KkusGlRtrys3Do3bJtafav4XnKypyjWgqrS34mm22vKltJShJxaPU9ONacTyldU3C4moJ8VzjaKC8vgIZJtCgjFujSlubXVFGNV63AQmqkVKL3i+ho+iLei3KgtMco0rW8rx52y/pMkpg+OWmNW0attWhUT6Umb9T1UdS3WYVUm72OtssxiqmEZeKuAAPWtfQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQA4bUVu3sv1CLks+P5ossvW06tzVjHmrfZsxrP8AqrhuTrCtUlWg6kU+sgrrXymbvG61e3tbhqDb4xZiK24xUjdp2qQ8MYNrsQyjVaQzevZdaOVZRsY17exqpvio81kKs96q4jmm7qSq15OMm3tuYjjOYLjFbidSrUlJt9bLO5NsjqsuMtU7adiubhvBtvsMQ/TZm7evutWlXqSnNtyfadYOYxbZic81IQAAyCBQnL+jCUn+ibL/AJeyzcYzdU6dKm5OT26CX2hPJhqYg6F1e0VzXtwkjI0dG+peA0LUL/iSjsUBlndt3KENTDsUqx/mMLva3+7tpy/yRZcWwXHbahUuLnB8Rtramt51q1pUhCK324ya2XSjehlHIGG5UsqVGhQp86KXHmouWacq4XnPLWKYDi9nTvMLxK2qWlzbzXCdOcWmv8enqfEnbDdRQ2Mtc+m1neJ1v42KjWNMX3LE73RRzFke4eK/P63uVFhht3itfyFlaV7yvs5eSt6Uqktl0vZJvYzfXfSDEdC9UsayfiHPqRtKnlLO5qLb9ptZN+Sq/wB6TT/tRkuot2kup+L6N6h4Lm/BWpXmG11OdvKTULmk+FSlL9JR3X6PZ9RaITfq0361Lk7MZt8Admzb4Z/ZVkMepNqT7Nu1Wr/QnMfs9i3uNX7TrucpY7Z0Kle4wTErehTXOnVq2dSMYrtbcdkjfHkTOOD6jZPwjM2CVY3OF4nbwuaE9uO0l0NdTT3TXamQi/E25QX8l4bZaVYFXjG4vYxvMbq03tKFFPelQ4fvveUv7MUv6zI+tmLaq5VraJtNkc9vxH4QO8nZ4fzsWxVNnhpoDMZdnhs79y1yHfY4fdYncxt7K1rXlxJNqlb05VJtLi3sk2dBs3/DS5PEMqZQranYzbf+L49RdDC1Vjxo2O6bnH9arSe/7sY9r33C9XaOzUhqXjM9wG8/92lYWho3Vs36Y2DxO5a4f9Ccx+z2Le41ftOynlDMtJ8MAxb3Gr9pv58nD92P/AeTh+7H/gRjJjz9ZpZJSgj/AFf/AFW2w2V1M8SQzFpHiP8A9Whi1wHMdJLymA4rFLrlZVV/6StVjf0l/PWF1R/3lCUf80b2ZUac4tOEWn1bGDZ70nwnN9nUjO3pqq09nzSJb9DQ3UmSGn/Td6O/srDYP0hXKxlsFbKZI/UbVpecJR6YuL/VHBNDWzksVsLlWuLGi3Di9kiKmY8n3mB3E6dajKDi9uKIeqqOSmdk4K5FjxJQ3yESU7wTuWNH1Gbi+BzKm4Pij4Mets71kGAZru8FrwqUasoc178GSb0b5VF3gdajRvK0nT4JtsiKnsd1C6nQknGTR7qerlpnazCtYvGHqG9RGOpYDmtwmnuuuD5voU4utBVJJbPc9Ot7mndU1OlNTi+tM0y5O1RxPLVzTnRuJxUX0bkstH+VtUjUo0b6vweye74G8UV8ZLk2XYVVfE+iyqoS6e3/ABN3KdoMPydqThea7enKlcU+fJbpc4y9NNbrijamPa8ZtKgSenlpXmOZuRC5ABzXnQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQHxWrQoU3OclGK62eVam65YXk6zqxjWh5XZ8dzqklZE3WeclkKKgqLhKIadhcSvQ8ZzLYYFbzq3VeMFFb7bkddXuVNYYJa1qFlWTqbNbJkadX+U9fZgrVaVtXlGnu+hkc8azPd4vWlUrVZSbfWzTK6+97IVZbCuirPVqbnyXo+pWt+JZsuau9xPybfBbnkt1e1LmblOTbfaU8puXSfJpcsz5nazyrM0Nup7dEIqdoAC5b3ODlJsrbDC617UUacHJv9DpAz7lkXODBm4qlp0pVJJJbmY5OyFe5ivadOnRlJSaXQej6U8n/ABHNV5RlOhJU5NcWic+k3JwwzK1CjVr0YuUUns1xbM/Q2mWpIJGQUSYq0gUNjYY43a0m4Ly7QXk007VW91e0VzuD2cegl5g+CWuCWkKFvTjBRW26RUWdhQsKSp0YKEVw4IqCRaWkjpWarAqZX3ENXfqgzVDtngEAB7lq6iR+Ilyeo6p6Yf6Y4Tayq5mytSnW5tGO87iz351WnsuMnHZziv0kl/Se+p1NNJp7p9aP0LyjGpFxklKLWzTW6aNNHLY5P/mD1juKOH27pZXxxTxDCmo7QprnfztBf7uTWy6ozgTNga8a7TbJjtG1n7eI/p3j+q0y/UfdVMHof+D/AML0fkQcsmz0NynmzLObK06+D0LeeJ4JTW/O/aePPtk+pVHzZLhwam+PO4RSzpnLF9Q82YtmXHrn9rxjFLid1c1Emo86T/oxTb2jFbRit3skkWY4lJRi2+hEkU9spqaqlrIm5Pkyz/pu/fvO87VrUtXLNCyBx2N7l67yWNCq3KE1fwzLc4VVgtBftuL16e68naxa3jzupzbUF18W10M3aYfh9thNhbWNnQhbWltSjRo0aUVGFOEUlGKS6EkktiPfIb5PS0J0ht62JW3kc15gjTvsT8pFKpRXN/m7d9nMUnuv3pSJGEDYrvHWlcWRn/Dj2D1Pif6+HoAt+tVH0SAaw+J20+yAA0pZpAAEVFieD2uLW86NzSjOMltxRHbWPky2OYLStXs7eKqbN7pEljhpSWzW67GeWemjqG6rws7ar1WWeYTUryMvBah9StD8Tyrc1W7efk03s9jyS6sKlrNxnFxaNzeedK8MzhZ1ac6MFOSfUQr1l5Klzh9SvcWdD8q3eyRodfZXw/HFtCtnhHSbTXECnrjqvUL2tjgynMmSb7AricK1CUdn1oxqpSlB7NbGquaWnIhT3DPHO0PjOYXwnsysssTrWdRSpzcWuxlEDiDl3Luc0OGTgvbtM9esTypc0l+0TcE1wcugm1pRypsOx6hQo3dWPPaSe74mrmNRxe6ZfcDzVd4PXhOjVlFrsZm6K6zUpyzzCi7EuArdfWF2rqv3hbrMIzBZY1RjUtqsZqS3WzLma1tHuVFe4LUo0Lmu3BNLdsm9pzrLhuc7GlLysfKNceJIFHcoqsbDkVUTEmCbhh95Lm6zN69LB8wmpxUovdPoaPoy6jpAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARADrrV4UIOVSSjFdbC+gZ7Auws2YM02OXraVS5rwg0t9mzAdStcsIyhbVYxuIucV07og5rFyl77MN1Xp21xKNNtpbS6jC1tzipRlnmVJeGcDXC/wAgcWlse8r3jW3lU0rBVbWxrreO6XNkQrz7qviWa7upOtcTkm3w3MPxjMFzilaU6tSUm31stMpOTI9rLjLVO2nYriYcwbb7BEBGwF29dle5nWk3Jts6QcpbmJUhAAbAuD6hBzfAqLWwqXU1GEXJvsR6pp1onima7qnzLafMbW75p2xQvlOqwLG1twp6CMyzvAAWD5ayhd47cwp0qUpc57cES70J5LtS9q0bm+t3zeDakuB63opyZLbAaNK6u6Meckv6USSuFYTb4RbRo0KcYJdOyN5t1lDcpJlVnGek102tSWw7O7NWLJ+n2GZUtYQoW8FNJLfboMpSSWyWyOQbi1jWDJoVbZ55al5kldmTvQAHNedAAEQ8Y5WegtDlB6QYjgdOEI49Z73uEV5bfkuIp7Q36ozW8H/HfqPZwemmqJKSZlRCcnNOYXXJG2VhjeNhX56q9CtaV6tC4o1Le4ozlTq0asXGdOcXtKMk+hppprqaJT/h68nzzu6rPM2K2/lMs5VqQrzU47wuLx8aVLj083bykuz8n7xmf4hnJhxHCtVsKzblHC6l7bZwuo2da0tof0MSfCP6JVVx36FKEm3xJ58nrRmw0G0pwXKdo6da4oU/K313CPN/abmSTqVO3bfgt+hJE13zE8Zs7JKU5PmGXq3zcu4fvmFpVDanNrHCUfCz77l6QACC1vKAAIgACIAAiFHiOE22KUnTuKammtuKKwHwgHYVya5zDrNORXgWp3JnwrM1OrUoUoqUuK2WzRDDVLkz4nluvWnRoSnCLfBLqNphZ8dyth+P0JU7m3hJtbbtGDrLTDUjMDIqU8OaQblZHBj3a7NxWlHF8u3WF1pQq0pRafWi0yi4vibKNZ+SrbYnGtc2NBJy3e8UQr1B0axPKt3VjO3koxfSkaFWW2alO0bFbbDmNrffoxqPAduK8pBU3VlUtpuM4uLRTtbGH7lIwIcMwu+3u50JJxbTXYeq6aay4jlO7pONeSgnxW/UeRH3Co4PdPY7YpXxO1mlY+ut9PXxGKdoIK2laJ8pGyzHZ0ra7uI8/ZL8zJD4fidvilvGtb1Y1IyW/BmlXK+db3ALmFSjWlDmvhsyWuiPKpqWc6Vtf13zXsvzM3q33sOyjmVVMYaMJIC6rtozHfkp/gw3JWpmFZutISo14+Ua6N1xMxjJSSae67TcGPa8azSq5VFNLSyGOZpBG9cgA5rzIAAiAAIgACIAAiAjF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwTO9RXb5WT6Hey83SYPOOYUnQRi9ZHoH7VYj8v4h4I9ZHoH7VYj8v4h4I6iu3ysn0O9k6TB5xzCk6CMXrI9A/arEfl/EPBHrI9A/arEfl/EPBHUV2+Vk+h3snSYPOOYUnQRi9ZHoH7VYj8v4h4I9ZHoH7VYj8v4h4I6iu3ysn0O9k6TB5xzCk6CMXrItA/arEfl/EPBHrI9A/arEfl/EPBHUV2+Vk+h3snSYPOOYUnQRjX4kOgkuEc04jJ9n8gX/gmNZq/ET03u7apHAsSvbibX5efhlzT/AO6mjH1dDWUDNephc0erSP5CzNsoZrvKIaMaxO5Snx/NmH5et51LmvCPNW7XOIr6z8qyjYxr29jVi3xUeayM2o/Kmvc3zqxtLmtGlPfpjKP+Z4jimP3GJ1pTq1JSb7WRtcL245siGStJhPRWyEtqbltO5ZnnvVXEc03dSVWvJxk29tzz2vczrTblJts6nJtnBpr5HSHNxVkaWjho4xHC3IBct7nAPuEqUX/OT5q7dmzg1pccmhetz2sGbjkFzClKo9kjLMq5Bv8AMVzCnRoylzn0pHXlbF8mWdzCWNYvUtqafHmWVep/2wZKHS/lG8nTJlCl+2ZjvHXjtzubgN/Lj/FUTc7ZhS63H446d5bvDSf+FD2K9I9ssLDG2QOk3ZhXTRzklVr2NG5vqO3Q9pImBkjSTCsn2tOFOjB1Elu1HrPGLP8AEU5PlhRjSoZmv6cIrZJZfv8AwDv9ZHoH7VYj8v4h4JI9LhKvpR8NJJn/AKHeyqBfscVd+kJmnAbuzCk3CEacVGKSiuCSPojF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwTJ9RXb5WT6Hey0bpUHnHMKToIxesj0D9qsR+X8Q8Eesj0D9qsR+X8Q8E+9RXb5WT6HeydJg845hSdBGL1kegftViPy/iHgj1kegftViPy/iHgjqK7fKyfQ72TpMHnHMKToIxesi0D9qsR+X8Q8Eesj0D9qsR+X8Q8EdRXb5WT6HeydJg845hSdBGL1kegftViPy/iHgj1kegftViPy/iHgjqK7fKyfQ72TpMHnHMKTFa2o3Pk/LUoVfJzVSHPinzZLokt+hrtO0jF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwR1FdvlZPod7J0mDzjmFJ0EYvWR6B+1WI/L+IeCPWR6B+1WI/L+IeCOort8rJ9DvZOkweccwpOgjF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwR1FdvlZPod7J0mDzjmFJ0EYvWR6B+1WI/L+IeCPWR6B+1WI/L+IeCOort8rJ9DvZOkweccwpOgjF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwR1FdvlZPod7J0mDzjmFJ0EYvWR6B+1WI/L+IeCPWRaB+1WI/L+IeCOort8rJ9DvZOkweccwpOgjF6yPQP2qxH5fxDwR6yPQP2qxH5fxDwR1FdvlZPod7J0mDzjmFJqpSjWg4zipRfUzAM86O4Rm+2mp0YKo11rpPJPWR6B+1WI/L+IeCPWR6B+1WI/L+IeCdT8PXOQZPpJD/sd7L2Ut16FIJYJg0jcQvFtZ+SdWw2VW4saLcOL/ACoijmfJN7gFzOnXoyhzX1o2G4h+Ihye8Ut5UbjMt/UjJbccvX/gHgmrev8AyfM705yw3Md35eXQpYFfQ3/vdE0+44HuZBkhpZPod7KxmENLLYnNpbnICO7PMKI06bg9mfBf8wYjlq6uJvCcRlc02/yuVtVp/wDdFFibg3+WW6/gRjU0c9I8snYWkbxkrYUFyprlEJqd4cDuK4T2KyyxGrZ1FKE3FrsZRA8YOSyTmhwyK920n12xDKt7RUq8pU1w2cuonno9ygcOzXbUqNaslOSS4vrNTVKtKnLdPYzfJOp93lG8p1YVpxjB78N2bBb7pJTODTtCiDF2AaO+ROkibqyb1uet7mndU1OlJTi+tHaQR0z5fWVsv29KlmG7uqUYraThZVqv/bFnqMfxINCFFeVzNiNOXXH+Qb9/5USV7fBU3NmvSxOd+zSf4CpbfbNUWCcw1YyG9SdBGL1kegftViPy/iHgj1kegftViPy/iHgmY6iu3ysn0O9lq3SYPOOYUnQRi9ZHoH7VYj8v4h4I9ZHoH7VYj8v4h4I6iu3ysn0O9k6TB5xzCk6CMXrI9A/arEfl/EPBHrI9A/arEfl/EPBHUV2+Vk+h3snSYPOOYUnQRi9ZHoH7VYj8v4h4I9ZHoH7VYj8v4h4I6iu3ysn0O9k6TB5xzC1CAAtMojQABEAARABGLk9ktzi5waM3HILk1jnnVaMyh3ULSdZrhwKqyw2VR7tF+tbGNJLhxIkxTj2ls7TFTHWkVgsC6KK/Eb21FY0si/lW+ywhR2bRd6VvGmkkjsUUjkqpesS196kL53nLcr54awVasNQiOljGe/xToABqXepAAyQdBxKaj0lDd4hGlF8eJlrfa6m5SiKBhJKwF2vlFZoHT1cgaAqqtcRpxb3LPe4sluosoLvEp1W0mULbk929yz2FdG0VKG1FwGbtypBjzTRPXF1HZzk3u1l2Vq8qz3b4HWAT1DDHTsEcQyAVT6mpmq5DLO4ucfEoADvXmQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARVNteSoSXHgXyzxSNRJN8TGj6hUlTe6ZH+IcHUN9jJLdV+9S1g/SNdcLStDXl0fiCs1hUU1wZ9mN2WKuLSky+W91GqlsypOIsIVtikOu3Nu9foHg7SHbMUwj9N4D/ABBVQAnuDQMiFLQII2L4qUlNNMs2IYYmm4ovh8ygpI26w4iq7JOJIXbNyj/FWD6DE9K6GoYM/ArCqtGVKTTR8GT3mGxqJtIsVzZSoyfDgXCw3jOivsYaXar9y/OrGeje54Wmc8N1ovAhUoAJEUOoAD6iAAIgACIAAiA5jBzeyRcrPCpTacjA3W9UdoiMtS8D0W1WHDVxxDOIKKMnPx8FR29nOs1w4F6ssJUUnJcSutrKNKK4FWkkVbxRpGqbg50FGdVivXgbQ5Q2dram4jXk9V10qEaa4I7ACEpZpJ3F8hzKszT00VMwRxNyAQA+J1VBbtnGOJ8rtVgzK5yzRwNL5DkF9t7HRWuoUk92UN5ikaaaT4lkub+dZ8HwJhwzo9rLq4S1A1WKu2N9L1usDXQUZ15fRXG9xfpUWWitcSrN7vgdfT08QWksuGqCyRhsDNu9UWxLja64nmL6qQhu4HYgANsWgIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIm+xWWl/Ki1u+BRgx1dQU9xiMNQ3MFZe13artFQKikeWuCyuzv41YriV0ZKS4GGULmVGS2Zf7DEVUSTZVPGOj+W2k1NGM2K+2jnS3BeWtobidWQfdXUHzCakuk+iCnsdGdVwVpo5GytDmHMFGtylubONVdBVA9lHWzUUglhdkQsfcLZTXOEwVLA4FYvfYa6cm4ot0ouL2ZmlWhGouKLLiGF9Lii0WD9IjKkNpLgcjvVGNIuh+Wic+4WkZt7y1WQH3VpSpS2aPgn+ORkrQ9hzBVSZoZKd5jlGTh4IADtXSgBzCEpvZLc4Oe1g1nHILnHG+VwYwZkrg77e0nWkuD2KyywuU2nJF8t7KNJLgQ9ijSDSWpphpTrPVjMD6Ia+/ObU14LIt29UVnhSgk2i606SprZI+0tugFU7vf6y8SmSoeSr6YewpbcOwCGkjAI8UABrXet07kDex11K0aa3bLZeYrGG6TNktNgrbvIGU7CVpl/xZbMPQmWrlA9FW3N5Ginx4livcVlOTUWUtxezrN8SmLVYV0fUtraJqxus9UOx5pcrr491NbnFke8eK+p1JVHu2fIBMbGNjbqsGQVcJJXzOL5DmTvQAHYutAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQ7KVZ0nwOsHVLEyZhZIMwV3wTyU0gliOTgr7Y4ouCky8Ua8aq3T3MKUnF7ouFliMqUkm+BAOLdHEVS11Vb9ju/JWz0f6ZZqJzKG7nNvcHLKgUlrexrRXEq09yr1bQT0EpinbkQry2y60t2gbPTPDgUPmcFNcUfQPCx7o3azTtWUkjZK0seMwrVfYaqibS4lhuLSVGXRwMzaTRR3VjGtF8Cb8I6QJ7Y5tPVnWYqw6QtEdNe2urLeNWX08ViILheYZKk24ooHBp7bFq7ddqS6QiameCFQy8WC4WOoNPWRkELut7SVd/oXuywpQ2bR6NmfHcMvbibs8IsbOG/BUKEY7f8ABGK1JqTbUYr+CKmYj0kVd2BiphqMV+8E6G6CxatTXf4kn2XRTpRguCPsAhqSV8rtZ5zKspDBHAwMjGQCA+4U3N7IyHLmTrzHriFOjRlNye3BHBrS45BJZo4Gl8hyAWNSkoriUl1fwpRfHiTR0e5IkMwVKNTFLCNWm9nKM48NiVWW+SDpfh1vH9tyZhN5V24+Wt1L/M3qyWmFzxJWNOruCr5jPSXFQMdTW5wL9+5aZ7zFXUbSf+JbpVOe93Lc3jei5pH3dZe9xh9Dn0XNI+7rL3uMPoWLtmKLTaYhFTUrhyVJL31pf5zPW1Gtn4bclo33Xahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+hm+39JwHcwtZ7PScQclo33Xahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+g7f0nAdzCdnpOIOS0b7rtQ3XajeR6Lmkfd1l73GH0HouaR93WXvcYfQdv6TgO5hOz0nEHJaN912obrtRvI9FzSPu6y97jD6D0XNI+7rL3uMPoO39JwHcwnZ6TiDktG+67UN12o3kei5pH3dZe9xh9B6Lmkfd1l73GH0Hb+k4DuYTs9JxByWjfddqG67UbyPRc0j7usve4w+g9FzSPu6y97jD6Dt/ScB3MJ2ek4g5LRvuu1DddqN5HouaR93WXvcYfQei5pH3dZe9xh9B2/pOA7mE7PScQclo33Xahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+g7f0nAdzCdnpOIOS0b7rtQ3XajeR6Lmkfd1l73GH0HouaR93WXvcYfQdv6TgO5hOz0nEHJaN912obrtRvI9FzSPu6y97jD6D0XNI+7rL3uMPoO39JwHcwnZ6TiDktG/OXahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+g7f0nAdzCdnpOIOS0b7rtQ3XajeR6Lmkfd1l73GH0HouaR93WXvcYfQdv6TgO5hOz0nEHJaN912obrtRvI9FzSPu6y97jD6D0XNI+7rL3uMPoO39JwHcwnZ6TiDktG+67UN12o3kei5pH3dZe9xh9B6Lmkfd1l73GH0Hb+k4DuYTs9JxByWjfddqG67UbyPRc0j7usve4w+g9FzSPu6y97jD6Dt/ScB3MJ2ek4g5LRvuu1DddqN5HouaR93WXvcYfQei5pH3dZe9xh9B2/pOA7mE7PScQclo33Xahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+g7f0nAdzCdnpOIOS0b85dqG67UbyPRc0j7usve4w+g9FzSPu6y97jD6Dt/ScB3MJ2ek4g5LRvuu1DddqN5HouaR93WXvcYfQei5pH3dZe9xh9B2/pOA7mE7PScQclo33Xahuu1G8j0XNI+7rL3uMPoPRc0j7usve4w+g7f0nAdzCdnpOIOS0b7rtQ3XajeR6Lmkfd1l73GH0HouaR93WXvcYfQdv6TgO5hOz0nEHJaN912obrtRvI9FzSPu6y97jD6D0XNI+7rL3uMPoO39JwHcwnZ6TiDktG+67UN12o3kei5pH3dZe9xh9B6Lmkfd1l73GH0Hb+k4DuYTs9JxByWjfddqHOXabyPRc0j7usve4w+g9FzSPu6y97jD6Hzt/ScB3MJ2ek4g5LSBa3sqEl+bh/Ev9jiUaqScjdB6Lmkfd1l73GH0LfjHJL0pxC1nChkbBbSq1wnRtYxf+BHmJK2z36Mno7mv37FMOCsS3XCkzWmbWi8RtWoGM1Nbo5Jt608kKxwOnWuMHw6nQhHd82nDYiTmTJ91gN1Up1aMoc19aK611E6lkIy2K+2HsT0d+gEkTxn4hY0D6lBxZ8mL7lufeuqtQjVT3RbKuFJzeyLyuB2RqRS/oxf9xtNqxHXWnMQPORWi37B1rxBkaqMEjxXxKTkfIPuFOU2kkaqt67l87blTaWFS6mowi5N9he8u5OvccuIU6NGU3J7cESr0T5LFbFJ0bi+otQ4PZoyFNRy1LsmBane8SUNjhMlQ8Z7l4rplodiebrultby5ja3bXUTi0a5Mlnl2jSuLujHnxX9ZdJ63kTSnC8n2dOMKMHVS4tIzqMVCKUVsl1EgUFojpwHP2lVBxXpGrby50NKdWNUeGYTbYTbqlb04wS7EVoBsQAGwKGXOc86zjmUAB9XFAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEQABEAARAAEVHieE22LW86NzSjUjJbcURl1s5M1pjNCvcWdBKfFrmolMfNSnGrFxnFST6meSopo6luq8LYLPfKyyziameR6LTpqDpHiOVryqp28lFN8djzW4tZ0JtSTTNxWo2i+E5wtK3+rR8pJdnWQP1o5OF7ly6r1aFvJ002+Eeoj64Wd9OdZm0K4GENI1LeGiCpOrJ6qL+2xwXTFcFr4bXlTqU3Fp9aLa4tM1kgg5FTex7ZBrNOYVZY4XWvakY04OTf6HtGmfJ+xTNNalOVvNUm1xa6T0fQfSDCMWuaNW4k5PdcHBE9MlZGwrLmG0Y21CHBLi4o2y22f9f45DsVfcaaR+qs6ajbm/evG9I+TJYZct6NW7oRdTbdtokJhmD2uE28KNtSjCMVtwRWbbHJvsFNHTt1WBVOul5rLvKZap5OaAA9KwaAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIgACIAAiAAIhj+aMn2WZrWdOvSi5NbbtGQA4uaHDIruilfA8SRnIhQf1v5KkpzrXNjQ6d5LmxIkY3pPimF4lVt5201KL6OazcjdWlK8pOnVgpxfU0eYZj0fwDEMTnXqUFz5Jb7I1Wtskcp149inrDGk+roI+j1g1gBsK//9k=
)"

; Decodes a base64 string to raw bytes and writes them to outFile.
Base64Decode(b64Str, outFile) {
    DllCall("crypt32\CryptStringToBinary", "Str", b64Str, "UInt", 0, "UInt", 0x1, "Ptr", 0, "UInt*", &outLen := 0, "Ptr", 0, "Ptr", 0)
    buf := Buffer(outLen)
    DllCall("crypt32\CryptStringToBinary", "Str", b64Str, "UInt", 0, "UInt", 0x1, "Ptr", buf, "UInt*", &outLen, "Ptr", 0, "Ptr", 0)
    f := FileOpen(outFile, "w")
    f.RawWrite(buf, outLen)
    f.Close()
}

; Decode once at startup into a temp file the Picture control can load from.
LogoFile := A_Temp "\new_logo_temp.png"
Base64Decode(LogoBase64, LogoFile)

; ══════════════════════════════════════════════════════════════
;  EDIT TEXT VERTICAL NUDGE
;  Plain Edit controls auto-center text vertically, but doing so with
;  the sunken 3D border removed (-E0x200) leaves the text sitting a
;  few pixels above where it visually should. EM_SETRECT lets us
;  redefine the text's drawing rect independent of the control's own
;  x/y/w/h — only works on multi-line edits, which is why the boxes
;  using this also get the Multi option (they still behave like a
;  single line since WantReturn is off and no scrollbars are added).
; ══════════════════════════════════════════════════════════════
ShiftEditTextDown(ctrl, w, h, offsetY) {
    rc := Buffer(16, 0)
    NumPut("Int", 2,       rc, 0)   ; left
    NumPut("Int", offsetY, rc, 4)   ; top   — pushes the text down
    NumPut("Int", w - 2,   rc, 8)   ; right
    NumPut("Int", h,       rc, 12)  ; bottom
    SendMessage(0xB3, 0, rc.Ptr, ctrl.Hwnd)  ; EM_SETRECT
}

; "-VScroll"/"-HScroll" in the Add() options string don't actually do
; anything for Edit controls (that negation only works on controls like
; ListBox) — the scrollbar is a window style baked in at creation, so it
; has to be stripped directly via the WinAPI afterward.
StripScrollbars(ctrl) {
    GWL_STYLE := -16
    WS_VSCROLL := 0x00200000
    WS_HSCROLL := 0x00100000
    style := DllCall("GetWindowLong", "Ptr", ctrl.Hwnd, "Int", GWL_STYLE, "Int")
    style := style & ~WS_VSCROLL & ~WS_HSCROLL
    DllCall("SetWindowLong", "Ptr", ctrl.Hwnd, "Int", GWL_STYLE, "Int", style)
    ; SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED — redraw the
    ; non-client area so the change actually takes visual effect
    DllCall("SetWindowPos", "Ptr", ctrl.Hwnd, "Ptr", 0, "Int", 0, "Int", 0, "Int", 0, "Int", 0, "UInt", 0x0027)
}

; ══════════════════════════════════════════════════════════════
;  GUI INITIALIZATION
; ══════════════════════════════════════════════════════════════
myGui := Gui("-Caption +Border", "jewdestroyer")
myGui.BackColor := BgColor
myGui.MarginX := 0
myGui.MarginY := 0
myGui.OnEvent("Close", CleanExit)

; Invisible control that keyboard focus gets shoved onto whenever you click
; away from a textbox — Text controls can't hold focus themselves, so
; without this, an Edit box would stay focused (and eating keystrokes)
; even after you click elsewhere.
focusSink := myGui.Add("Edit", "x0 y0 w1 h1 Hidden")

; --- CUSTOM TITLE BAR ---
titleBar := myGui.Add("Text", "x0 y0 w420 h30 Background" BarColor)
titleBar.OnEvent("Click", (*) => PostMessage(0xA1, 2,,, "A"))

; --- MINIMIZE & EXIT BUTTONS ---
minBtn := myGui.Add("Text", "x420 y-18 w30 h55 Center +0x200 Background" BarColor, "_")
minBtn.SetFont("s10 Bold c" TextMain)
minClick := myGui.Add("Text", "x420 y0 w30 h30 BackgroundTrans")
minClick.OnEvent("Click", (*) => myGui.Minimize())

closeBtn := myGui.Add("Text", "x450 y0 w30 h30 Center +0x200 Background" BarColor, "X")
closeBtn.SetFont("s10 Bold c" TextMain)
closeBtn.OnEvent("Click", CleanExit)

; --- PURPLE LINE ---
myGui.Add("Text", "x0 y29 w480 h1 Background" TintColor)

; --- TITLE TEXT ---
myGui.SetFont("s9 Bold c" TextMain, "Segoe UI")
myGui.Add("Text", "x10 y7 w200 +BackgroundTrans", "jewdestroyer")

; ══════════════════════════════════════════════════════════════
;  SIDEBAR INITIALIZATION
; ══════════════════════════════════════════════════════════════
CONT_X := 130
myGui.Add("Text", "x0 y30 w" CONT_X " h500 Background" BarColor, "")

; --- LOGO / IMAGE (decoded from embedded base64 above) ---
myGui.Add("Picture", "x35 y40 w60 h44", LogoFile)

; NOTE: "Hotkeys" stays at the bottom of the sidebar list.
tab_labels := ["Macros", "Z-Boost", "Click Boost", "Hotkeys"]
global tab_buttons := []

loop tab_labels.Length {
    btn := myGui.Add("Text", "x10 y" (60 + (A_Index * 40)) " w110 h30 +0x200 Center c" TextMain, tab_labels[A_Index])
    btn.OnEvent("Click", SwitchTab)
    tab_buttons.Push(btn)
}

; ══════════════════════════════════════════════════════════════
;  MACROS TAB CONTENT   (tab index 1 — also hosts Skill Spammer)
; ══════════════════════════════════════════════════════════════
CONTENT_X := CONT_X + 15

; --- Title ---
myGui.SetFont("s7 Bold c" TintColor, "Segoe UI")
tabContentControls[1].Push(myGui.Add("Text", "x" CONTENT_X " y50 w300 h10 +BackgroundTrans", "SORU"))

; --- Move(s) to press ---
myGui.SetFont("s9 Bold c" TextMain, "Segoe UI")
keyLabel := myGui.Add("Text", "x" CONTENT_X " y66 w300 h18 +BackgroundTrans", "Move(s) to press:")
tabContentControls[1].Push(keyLabel)

keyEdit := myGui.Add("Edit", "x" CONTENT_X " y86 w280 h24 Multi -WantReturn -VScroll -HScroll -E0x200 Background" ActiveTabColor " c" TextMain)
ShiftEditTextDown(keyEdit, 280, 24, 3)
tabContentControls[1].Push(keyEdit)
g_FocusableEdits.Push(keyEdit)

myGui.SetFont("s7 c888888", "Segoe UI")
keyHint := myGui.Add("Text", "x" CONTENT_X " y119 w300 h32 +BackgroundTrans",
    "Example: Z,XZ,C (cycle with " FriendlyKeyName(cycleGroupKey) ")`n")
tabContentControls[1].Push(keyHint)

rSpamCheckbox := ThemedCheckbox(myGui, CONTENT_X, 144, 110, "Spam R", false)
tabContentControls[1].Push(rSpamCheckbox.box)
tabContentControls[1].Push(rSpamCheckbox.label)
rSpamCheckbox.OnEvent("Click", RSpamCheckboxChanged)
keyEdit.OnEvent("Change", UpdateKeyGroups)

; --- Coordinate picker button — sets px/py by hunting for the #FFFFFF soru pixel ---
myGui.SetFont("s8 Bold c" TextMain, "Segoe UI")
global coordPickBtn := myGui.Add("Text",
    "x" (CONTENT_X + 150) " y142 w130 h20 +0x200 +Border Center Background" ActiveTabColor " c" TextMain,
    "Coords: (" px ", " py ")")
tabContentControls[1].Push(coordPickBtn)
coordPickBtn.OnEvent("Click", StartCoordPicker)

tabContentControls[1].Push(myGui.Add("Text", "x" CONTENT_X " y169 w300 h1 Background" DividerColor, ""))

; --- Skill Spammer (merged in from its old standalone tab) ---
myGui.SetFont("s7 Bold c" TintColor, "Segoe UI")
tabContentControls[1].Push(myGui.Add("Text", "x" CONTENT_X " y187 w300 h10 +BackgroundTrans", "SKILL SPAMMER"))

myGui.SetFont("s9 Bold c" TextMain, "Segoe UI")
tabContentControls[1].Push(myGui.Add("Text", "x" CONTENT_X " y206 w300 h18 +BackgroundTrans", "Move(s) to spam:"))

spamKeyEdit := myGui.Add("Edit", "x" CONTENT_X " y226 w280 h24 Multi -WantReturn -VScroll -HScroll -E0x200 Background" ActiveTabColor " c" TextMain)
ShiftEditTextDown(spamKeyEdit, 280, 24, 3)
tabContentControls[1].Push(spamKeyEdit)
g_FocusableEdits.Push(spamKeyEdit)

myGui.SetFont("s7 c" DimText, "Segoe UI")
spamModeHint := myGui.Add("Text", "x" CONTENT_X " y259 w300 h16 +BackgroundTrans",
    "Spams a move (hold " FriendlyKeyName(spammerTriggerKey) ")")
tabContentControls[1].Push(spamModeHint)

spammerCB := ThemedCheckbox(myGui, CONTENT_X, 286, 140, "Spam Enabled", false)
tabContentControls[1].Push(spammerCB.box)
tabContentControls[1].Push(spammerCB.label)
spammerCB.OnEvent("Click", SpammerCheckboxChanged)

spamKeyEdit.OnEvent("Change", UpdateSpammerKey)

; ══════════════════════════════════════════════════════════════
;  Z-BOOST TAB CONTENT   (tab index 2)
;  CLICK BOOST TAB CONTENT   (tab index 3 — identical layout,
;  built by the same function, just pointed at the other module)
; ══════════════════════════════════════════════════════════════
BuildBoostTab(2, zBoost)
BuildBoostTab(3, clickBoost)

BuildBoostTab(tabIdx, m) {
    global myGui, tabContentControls, CONTENT_X, TintColor, DimText, ActiveTabColor, TextMain, DividerColor

    X  := CONTENT_X
    W  := 315
    VX := X + 130
    VW := 80
    UX := VX + VW + 8

    ; --- Status strip: boost status / roblox status / enabled ---
    m.dotLabel := myGui.Add("Text", "x" X " y54 w9 h9 +0x200 Background22CC55", "")
    tabContentControls[tabIdx].Push(m.dotLabel)
    myGui.SetFont("s8 Bold c22CC55", "Segoe UI")
    m.textLabel := myGui.Add("Text", "x" (X+13) " y53 w85 h14 +BackgroundTrans c22CC55", "Boost Ready")
    tabContentControls[tabIdx].Push(m.textLabel)

    m.rbxDotLabel := myGui.Add("Text", "x" (X+108) " y54 w9 h9 +0x200 Background" TintColor, "")
    tabContentControls[tabIdx].Push(m.rbxDotLabel)
    myGui.SetFont("s8 Bold c" TintColor, "Segoe UI")
    m.rbxTextLabel := myGui.Add("Text", "x" (X+121) " y53 w90 h14 +BackgroundTrans c" TintColor, "Roblox Found")
    tabContentControls[tabIdx].Push(m.rbxTextLabel)

    m.enabledCB := ThemedCheckbox(myGui, X+220, 53, 90, "Enabled", m.enabled)
    tabContentControls[tabIdx].Push(m.enabledCB.box)
    tabContentControls[tabIdx].Push(m.enabledCB.label)
    m.enabledCB.OnEvent("Click", (ctrl, *) => m.ToggleEnabled(ctrl))

    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y76 w" W " h1 Background" DividerColor, ""))

    ; --- Session boost counter ---
    myGui.SetFont("s7 Bold c" DimText, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y84 w150 h12 +BackgroundTrans", "SESSION BOOSTS"))

    myGui.SetFont("s18 Bold c" TintColor, "Consolas")
    m.countLabel := myGui.Add("Text", "x" X " y96 w70 h26 +BackgroundTrans", "0")
    tabContentControls[tabIdx].Push(m.countLabel)

    myGui.SetFont("s7 c" DimText, "Segoe UI")
    m.sessionTimerLabel := myGui.Add("Text", "x" (X+150) " y88 w100 h12 +BackgroundTrans Right", "session: 0m 0s")
    tabContentControls[tabIdx].Push(m.sessionTimerLabel)

    clearBtn := myGui.Add("Text", "x" (X+150) " y102 w100 h18 +0x200 +Border Background" ActiveTabColor " c" DimText " Center", "clear")
    clearBtn.SetFont("s7 c" DimText, "Segoe UI")
    clearBtn.OnEvent("Click", (*) => m.ClearCounter())
    tabContentControls[tabIdx].Push(clearBtn)

    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y126 w" W " h1 Background" DividerColor, ""))

    ; --- SETTINGS section ---
    myGui.SetFont("s7 Bold c" TintColor, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y134 w" W " h10 +BackgroundTrans", "SETTINGS"))
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y146 w" W " h1 Background" DividerColor, ""))

    ; Boost Key — trigger key (click to rebind)
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y154 w130 h18 +BackgroundTrans", "Boost Key"))
    myGui.SetFont("s9 Bold c" TintColor, "Consolas")
    m.keyBox := myGui.Add("Text", "x" VX " y152 w" VW " h20 +Border +0x200 Background" ActiveTabColor " Center c" TintColor, StrUpper(m.boostKey))
    m.keyBox.OnEvent("Click", (*) => m.StartRebind("trigger"))
    tabContentControls[tabIdx].Push(m.keyBox)
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y176 w" W " h1 Background" DividerColor, ""))

    ; Boosted Key (Z-Boost) / Boost Action (Click Boost — fixed, not rebindable)
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y184 w130 h18 +BackgroundTrans", m.useClick ? "Boost Action" : "Boosted Key"))
    zkColor := m.useClick ? DimText : TintColor
    myGui.SetFont("s9 Bold c" zkColor, "Consolas")
    m.zKeyBox := myGui.Add("Text", "x" VX " y182 w" VW " h20 +Border +0x200 Background" ActiveTabColor " Center c" zkColor, m.useClick ? "CLICK" : StrUpper(m.zKey))
    if (!m.useClick)
        m.zKeyBox.OnEvent("Click", (*) => m.StartRebind("sendkey"))
    tabContentControls[tabIdx].Push(m.zKeyBox)
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y206 w" W " h1 Background" DividerColor, ""))

    ; Hold Time
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y214 w130 h18 +BackgroundTrans", "Hold Time"))
    m.holdEdit := myGui.Add("Edit", "x" VX " y215 w" VW " h20 Center -E0x200 Background" ActiveTabColor " c" TextMain, m.holdTime)
    m.holdEdit.OnEvent("Change", (ctrl, *) => ForceDigitsOnly(ctrl))
    tabContentControls[tabIdx].Push(m.holdEdit)
    g_FocusableEdits.Push(m.holdEdit)
    myGui.SetFont("s8 c" DimText, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" UX " y215 w30 h14 +BackgroundTrans", "ms"))
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y236 w" W " h1 Background" DividerColor, ""))

    ; Lag Start Delay
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y244 w130 h18 +BackgroundTrans", "Lag Start"))
    m.lagStartEdit := myGui.Add("Edit", "x" VX " y245 w" VW " h20 Center -E0x200 Background" ActiveTabColor " c" TextMain, m.lagStart)
    m.lagStartEdit.OnEvent("Change", (ctrl, *) => ForceDigitsOnly(ctrl))
    tabContentControls[tabIdx].Push(m.lagStartEdit)
    g_FocusableEdits.Push(m.lagStartEdit)
    myGui.SetFont("s8 c" DimText, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" UX " y245 w30 h14 +BackgroundTrans", "ms"))
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y266 w" W " h1 Background" DividerColor, ""))

    ; Lag Duration
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y274 w130 h18 +BackgroundTrans", "Lag Duration"))
    m.lagDurEdit := myGui.Add("Edit", "x" VX " y275 w" VW " h20 Center -E0x200 Background" ActiveTabColor " c" TextMain, m.lagDuration)
    m.lagDurEdit.OnEvent("Change", (ctrl, *) => ForceDigitsOnly(ctrl))
    tabContentControls[tabIdx].Push(m.lagDurEdit)
    g_FocusableEdits.Push(m.lagDurEdit)
    myGui.SetFont("s8 c" DimText, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" UX " y275 w30 h14 +BackgroundTrans", "ms"))
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y296 w" W " h1 Background" DividerColor, ""))

    ; Intensity
    myGui.SetFont("s9 c" TextMain, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y304 w130 h18 +BackgroundTrans", "Intensity"))
    m.intensityEdit := myGui.Add("Edit", "x" VX " y305 w" VW " h20 Center -E0x200 Background" ActiveTabColor " c" TextMain, m.intensity)
    m.intensityEdit.OnEvent("Change", (ctrl, *) => ForceDigitsOnly(ctrl))
    tabContentControls[tabIdx].Push(m.intensityEdit)
    g_FocusableEdits.Push(m.intensityEdit)
    myGui.SetFont("s8 c" DimText, "Segoe UI")
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" UX " y305 w30 h14 +BackgroundTrans", "/100"))
    tabContentControls[tabIdx].Push(myGui.Add("Text", "x" X " y326 w" W " h1 Background" TintColor, ""))

    ; --- Apply / Reset buttons ---
    myGui.SetFont("s9 Bold c" TextMain, "Segoe UI")
    applyBtn := myGui.Add("Text", "x" X " y336 w150 h28 +0x200 Background" TintColor " Center", "Apply")
    applyBtn.OnEvent("Click", (*) => m.ApplySettings())
    tabContentControls[tabIdx].Push(applyBtn)

    resetBtn := myGui.Add("Text", "x" (X+160) " y336 w150 h28 +0x200 +Border Background" ActiveTabColor " Center c" TextMain, "Reset")
    resetBtn.OnEvent("Click", (*) => m.ResetSettings())
    tabContentControls[tabIdx].Push(resetBtn)

    ; --- Status bar ---
    myGui.SetFont("s7 c" DimText, "Segoe UI")
    m.statusBar := myGui.Add("Text", "x" X " y372 w" W " h14 +BackgroundTrans", "Ready — press " StrUpper(m.boostKey) " to boost")
    tabContentControls[tabIdx].Push(m.statusBar)
}

; ══════════════════════════════════════════════════════════════
;  HOTKEYS TAB CONTENT   (tab index 4)
; ══════════════════════════════════════════════════════════════
HK_X := CONTENT_X
HK_W := 300

HK_VX := HK_X + 160
HK_VW := 80

myGui.SetFont("s7 Bold c" TintColor, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y50 w300 h18 +BackgroundTrans", "GLOBAL HOTKEYS"))

myGui.SetFont("s9 c" TextMain, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y70 w" HK_W " h18 +BackgroundTrans", "F2 — Reload script"))
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y92 w" HK_W " h18 +BackgroundTrans", "F3 — Pause / resume all hotkeys"))

myGui.SetFont("s7 c" DimText, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y118 w" HK_W " h32 +BackgroundTrans",
    "When paused, only F2 and F3 hotkeys stay active."))

; --- Divider separating the fixed F2/F3 hotkeys from the editable ones below ---
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y142 w" HK_W " h1 Background" DividerColor, ""))

myGui.SetFont("s7 Bold c" TintColor, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y160 w" HK_W " h10 +BackgroundTrans", "EDITABLE HOTKEYS"))


; --- Skill Spammer trigger key (click to rebind) ---
myGui.SetFont("s9 c" TextMain, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y183 w150 h18 +BackgroundTrans", "Skill Spammer"))
myGui.SetFont("s9 Bold c" TintColor, "Consolas")
spammerTriggerBox := myGui.Add("Text", "x" HK_VX " y183 w" HK_VW " h20 +Border +0x200 Background" ActiveTabColor " Center c" TintColor, FriendlyKeyName(spammerTriggerKey))
spammerTriggerBox.OnEvent("Click", (*) => StartHotkeyRebind("spammer"))
tabContentControls[4].Push(spammerTriggerBox)

; --- Cycle Groups key (click to rebind) ---
myGui.SetFont("s9 c" TextMain, "Segoe UI")
tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y214 w150 h18 +BackgroundTrans", "Cycle Skill"))
myGui.SetFont("s9 Bold c" TintColor, "Consolas")
cycleKeyBox := myGui.Add("Text", "x" HK_VX " y214 w" HK_VW " h20 +Border +0x200 Background" ActiveTabColor " Center c" TintColor, FriendlyKeyName(cycleGroupKey))
cycleKeyBox.OnEvent("Click", (*) => StartHotkeyRebind("cycle"))
tabContentControls[4].Push(cycleKeyBox)

myGui.SetFont("s7 c" DimText, "Segoe UI")
hkRebindHint := myGui.Add("Text", "x" HK_X " y251 w" HK_W " h28 +BackgroundTrans", "Click a box, then press a key. Esc cancels.")
tabContentControls[4].Push(hkRebindHint)

tabContentControls[4].Push(myGui.Add("Text", "x" HK_X " y274 w" HK_W " h1 Background" DividerColor, ""))

hkStatusDot := myGui.Add("Text", "x" HK_X " y295 w9 h9 +0x200 Background22CC55", "")
tabContentControls[4].Push(hkStatusDot)
myGui.SetFont("s8 Bold c22CC55", "Segoe UI")
hkStatusLabel := myGui.Add("Text", "x" (HK_X+13) " y294 w200 h14 +BackgroundTrans c22CC55", "Hotkeys Active")
tabContentControls[4].Push(hkStatusLabel)

; Set initial active state
SwitchTab(tab_buttons[1])

myGui.Show("w480 h530")
ControlFocus(focusSink)

; Load saved config (if any), sync UI, register hotkeys for both boosts
LoadAllConfig()
zBoost.CalcTimings()
clickBoost.CalcTimings()
zBoost.RegisterHotkey()
clickBoost.RegisterHotkey()

SetTimer(UpdateAllSessionTimers, 1000)
SetTimer(PollAllRoblox, 2000)

SaveAllConfig()      ; make sure config.ini exists/is current before the engine reads it
EnsureMacroEngine()

; ══════════════════════════════════════════════════════════════
;  MACRO ENGINE PROCESS  (native soru.exe, pulled from GitHub)
;
;  EnsureMacroEngine() runs once at startup:
;    1. tries to fetch dist/version.txt from RemoteBase
;    2. if that differs from the cached version.txt in %AppData%
;       (or engine.exe isn't there yet), downloads the new engine.exe
;    3. if GitHub is unreachable, just falls back to whatever's
;       already cached locally — no internet required after the
;       first successful run
;    4. launches engine.exe hidden, same as before
; ══════════════════════════════════════════════════════════════
EnsureMacroEngine() {
    global EngineDir, EnginePath, EngineVersionPath, RemoteBase

    if !DirExist(EngineDir)
        DirCreate(EngineDir)

    tempVersionPath := EngineDir "\version.remote.txt"
    remoteVersion := ""
    try {
        Download(RemoteBase "version.txt", tempVersionPath)
        remoteVersion := Trim(FileRead(tempVersionPath))
    } catch {
        ; GitHub unreachable / no internet — fine, we'll use the local cache below
    }

    localVersion := FileExist(EngineVersionPath) ? Trim(FileRead(EngineVersionPath)) : ""
    needsDownload := !FileExist(EnginePath) || (remoteVersion != "" && remoteVersion != localVersion)

    if (needsDownload && remoteVersion != "") {
        try {
            Download(RemoteBase "soru.exe", EnginePath ".new")
            FileMove(EnginePath ".new", EnginePath, 1)   ; atomic-ish swap so a half download never becomes "the" exe
            FileCopy(tempVersionPath, EngineVersionPath, 1)
        } catch as e {
            MsgBox "Couldn't download the macro engine update: " e.Message
                . "`nWill use the last cached copy if there is one.", "Update failed", 48
        }
    }

    try FileDelete(tempVersionPath)

    if !FileExist(EnginePath) {
        MsgBox "No macro engine is cached and none could be downloaded — the R-macro won't run this session."
            . "`nCheck your internet connection, or that RemoteBase in the script points at your GitHub repo.",
            "Macro engine missing", 48
        return
    }

    LaunchMacroEngine()
}

LaunchMacroEngine() {
    global g_MacroEnginePID, EnginePath, EngineDir, ConfigFile
    try {
        myPID := ProcessExist()
        cmd := '"' EnginePath '" "' ConfigFile '" ' myPID
        Run(cmd, EngineDir, "Hide", &pid)
        g_MacroEnginePID := pid
    } catch as e {
        MsgBox "Couldn't start engine.exe: " e.Message, "Error", 16
    }
}

UpdateAllSessionTimers() {
    global zBoost, clickBoost
    zBoost.UpdateSessionTimer()
    clickBoost.UpdateSessionTimer()
}

PollAllRoblox() {
    global zBoost, clickBoost
    zBoost.PollRoblox()
    clickBoost.PollRoblox()
}

; ══════════════════════════════════════════════════════════════
;  UNIFIED CONFIG  (covers Macros, Spammer, Z-Boost, Click Boost)
; ══════════════════════════════════════════════════════════════
SaveAllConfig() {
    global ConfigFile, keyEdit, rSpamCheckbox, spamKeyEdit, spammerCB, zBoost, clickBoost, g_LoadingConfig
    global spammerTriggerKey, cycleGroupKey, px, py, HotkeysSuspended
    if (g_LoadingConfig)
        return  ; don't save while we're still loading — fields aren't all populated yet
    IniWrite keyEdit.Text,                  ConfigFile, "Macros", "MoveKeys"
    IniWrite (rSpamCheckbox.Value ? 1 : 0), ConfigFile, "Macros", "RSpamEnabled"
    IniWrite px,                            ConfigFile, "Macros", "PixelX"
    IniWrite py,                            ConfigFile, "Macros", "PixelY"
    IniWrite spamKeyEdit.Text,              ConfigFile, "Spammer", "Key"
    IniWrite (spammerCB.Value ? 1 : 0),     ConfigFile, "Spammer", "Enabled"
    IniWrite spammerTriggerKey,             ConfigFile, "Hotkeys", "SpammerTrigger"
    IniWrite cycleGroupKey,                 ConfigFile, "Hotkeys", "CycleKey"
    ; Always keep [State] Suspended in sync with our real in-memory state —
    ; otherwise a stale "Suspended=1" left over from a previous session
    ; (e.g. the script closed while paused) keeps soru.exe frozen forever,
    ; since only F3's ToggleSuspend() used to touch this key.
    IniWrite (HotkeysSuspended ? 1 : 0),    ConfigFile, "State", "Suspended"
    zBoost.SaveConfig()
    clickBoost.SaveConfig()
}

LoadAllConfig() {
    global ConfigFile, keyEdit, rSpamCheckbox, spamKeyEdit, spammerCB
    global spammerKey, zBoost, clickBoost, g_LoadingConfig
    global spammerTriggerKey, cycleGroupKey, spammerTriggerBox, cycleKeyBox
    global px, py, coordPickBtn

    ; Block SaveAllConfig() for the whole load — several controls below fire
    ; Change/Click callbacks that call SaveAllConfig() when set programmatically,
    ; and we don't want that firing until every field has its real saved value.
    g_LoadingConfig := true

    ; Boost settings load/sync regardless (they have their own defaults)
    zBoost.LoadConfig()
    clickBoost.LoadConfig()
    zBoost.SyncUIFromState()
    clickBoost.SyncUIFromState()

    if FileExist(ConfigFile) {
        keyEdit.Text := IniRead(ConfigFile, "Macros", "MoveKeys", "")
        UpdateKeyGroups(keyEdit)
        rSpamCheckbox.Value := Integer(IniRead(ConfigFile, "Macros", "RSpamEnabled", 0)) ? 1 : 0
        px := Integer(IniRead(ConfigFile, "Macros", "PixelX", px))
        py := Integer(IniRead(ConfigFile, "Macros", "PixelY", py))
        coordPickBtn.Text := "Coords: (" px ", " py ")"

        spamKeyEdit.Text := IniRead(ConfigFile, "Spammer", "Key", "")
        spammerKey := Trim(spamKeyEdit.Text)
        spammerCB.Value := Integer(IniRead(ConfigFile, "Spammer", "Enabled", 0)) ? 1 : 0

        spammerTriggerKey := IniRead(ConfigFile, "Hotkeys", "SpammerTrigger", spammerTriggerKey)
        cycleGroupKey     := IniRead(ConfigFile, "Hotkeys", "CycleKey",       cycleGroupKey)
    }

    spammerTriggerBox.Value := FriendlyKeyName(spammerTriggerKey)
    cycleKeyBox.Value       := FriendlyKeyName(cycleGroupKey)
    UpdateHotkeyHints()

    g_LoadingConfig := false
}

; ══════════════════════════════════════════════════════════════
;  FUNCTIONS & HOTKEYS
; ══════════════════════════════════════════════════════════════

#SuspendExempt True
F2::Reload()
F3::ToggleSuspend()
~LButton:: SetTimer(DefocusTextBoxes, -1)
#SuspendExempt False

; Click anywhere that isn't the currently-focused textbox and it loses
; keyboard focus, so your hotkeys/typing stop landing in it. This also
; has to cover clicks OUTSIDE the gui entirely (e.g. clicking back into
; Roblox) — a background window still remembers which control had focus,
; so leaving the WinActive check in place meant clicking away from the
; script did nothing and the edit box kept eating keystrokes.
DefocusTextBoxes() {
    global myGui, g_FocusableEdits, focusSink
    try
        focused := ControlGetFocus(myGui)
    catch
        return

    isTrackedEdit := false
    for ctrl in g_FocusableEdits {
        if (focused = ctrl.Hwnd) {
            isTrackedEdit := true
            break
        }
    }
    if !isTrackedEdit
        return

    MouseGetPos(&mx, &my, &winHwnd, &ctrlHwnd, 2)
    if (winHwnd = myGui.Hwnd && ctrlHwnd = focused)
        return
    ControlFocus(focusSink)
}

ToggleSuspend(*) {
    global HotkeysSuspended, ConfigFile
    HotkeysSuspended := !HotkeysSuspended
    Suspend(HotkeysSuspended ? 1 : 0)
    ; macro_engine.ahk is a separate process with its own hotkeys, so this
    ; script's Suspend() call doesn't reach it — tell it via config.ini instead.
    IniWrite (HotkeysSuspended ? 1 : 0), ConfigFile, "State", "Suspended"
    UpdateSuspendStatusUI()
    ToolTip(HotkeysSuspended ? "Paused" : "Unpaused")
    SetTimer(() => ToolTip(), -1200)
}

UpdateSuspendStatusUI() {
    global HotkeysSuspended, hkStatusDot, hkStatusLabel
    if (HotkeysSuspended) {
        hkStatusDot.Opt("BackgroundCC2222")
        hkStatusLabel.Value := "Hotkeys Suspended"
        hkStatusLabel.SetFont("s8 Bold cCC2222", "Segoe UI")
    } else {
        hkStatusDot.Opt("Background22CC55")
        hkStatusLabel.Value := "Hotkeys Active"
        hkStatusLabel.SetFont("s8 Bold c22CC55", "Segoe UI")
    }
}

SwitchTab(btnObj, *) {
    global tab_buttons, tabContentControls
    target := btnObj

    tabIndex := 0
    for i, btn in tab_buttons {
        if (btn == target) {
            btn.Opt("Background" ActiveTabColor)
            tabIndex := i
        } else {
            btn.Opt("Background" BarColor)
        }
        btn.Value := btn.Value
    }

    if (tabIndex = 0)
        return

    for idx, ctrls in tabContentControls {
        for ctrl in ctrls
            ctrl.Visible := (idx = tabIndex)
    }
}

; ══════════════════════════════════════════════════════════════
;  MACRO TAB LOGIC
; ══════════════════════════════════════════════════════════════

RSpamCheckboxChanged(ctrl, *) {
    global rSpamEnabled
    rSpamEnabled := ctrl.Value
    SaveAllConfig()
}

; ══════════════════════════════════════════════════════════════
;  COORDINATE PICKER  (Macros tab — "Coords" button)
;  Focuses Roblox, then follows the cursor with a tooltip until you
;  click while hovering a pixel that matches targetColor (#FFFFFF).
;  Any click on a non-matching pixel is ignored — picking stays open.
; ══════════════════════════════════════════════════════════════
StartCoordPicker(*) {
    global g_PickingCoords, coordPickBtn, BoostProcessName, TintColor

    if (g_PickingCoords)
        return

    if !WinExist("ahk_exe " BoostProcessName) {
        MsgBox "Roblox isn't running — start it first, then set coordinates.", "Roblox Not Found", 48
        return
    }
    WinActivate("ahk_exe " BoostProcessName)
    WinWaitActive("ahk_exe " BoostProcessName,, 1)

    g_PickingCoords := true
    coordPickBtn.Text := "Picking... (Esc)"
    coordPickBtn.Opt("Background" TintColor)

    ; Same trick BoostModule uses for hotkey rebinding — clears "already
    ; pressed" history so the click that opened this doesn't immediately
    ; register as the confirm click.
    Sleep 150
    BoostModule.DrainKeyState()
    SetTimer(PollCoordPicker, 15)
}

PollCoordPicker() {
    global g_PickingCoords, px, py, targetColor, coordPickBtn

    if !g_PickingCoords {
        SetTimer(PollCoordPicker, 0)
        return
    }

    ; Esc cancels and keeps the old coordinates
    if (DllCall("GetAsyncKeyState", "Int", 0x1B, "Short") & 0x8001) {
        CancelCoordPicker()
        return
    }

    MouseGetPos(&mx, &my)

    color := 0
    try color := PixelGetColor(mx, my)
    isMatch := (color = targetColor)

    hint := isMatch
        ? "X: " mx "  Y: " my "`n#" Format("{:06X}", color) "  ✓ match — click to set"
        : "X: " mx "  Y: " my "`n#" Format("{:06X}", color) "`nHover the soru button (#FFFFFF)"
    ToolTip(hint, mx + 18, my + 18)

    ; Left click only confirms while hovering a matching pixel — anything
    ; else is ignored and picking stays open.
    if (DllCall("GetAsyncKeyState", "Int", 0x01, "Short") & 0x8001) {
        if (isMatch)
            ConfirmCoordPicker(mx, my)
    }
}

ConfirmCoordPicker(mx, my) {
    global g_PickingCoords, px, py, coordPickBtn, ActiveTabColor

    SetTimer(PollCoordPicker, 0)
    g_PickingCoords := false
    px := mx
    py := my
    SaveAllConfig()

    coordPickBtn.Opt("Background" ActiveTabColor)
    coordPickBtn.Text := "Coords: (" px ", " py ")"

    ToolTip("Coordinates set!`nX: " px "  Y: " py, mx + 18, my + 18)
    SetTimer(() => ToolTip(), -900)
}

CancelCoordPicker() {
    global g_PickingCoords, px, py, coordPickBtn, ActiveTabColor

    SetTimer(PollCoordPicker, 0)
    g_PickingCoords := false
    ToolTip()
    coordPickBtn.Opt("Background" ActiveTabColor)
    coordPickBtn.Text := "Coords: (" px ", " py ")"
}

; Forces an Edit control's text to uppercase as you type, without
; kicking the caret back to the start of the field.
ForceUppercase(ctrl) {
    upper := StrUpper(ctrl.Text)
    if (upper == ctrl.Text)
        return
    ctrl.Text := upper
    ; EM_SETSEL — put the caret back at the end of the text
    PostMessage(0xB1, StrLen(upper), StrLen(upper), , ctrl.Hwnd)
}

; Used instead of the Edit control's native "Number" option — that option
; maps to the real ES_NUMBER style, which on modern Windows causes the OS
; to silently attach its own spin-button (up/down) control next to the
; box, showing up as a stray light-grey strip that doesn't match the
; theme. Filtering manually on Change gets the same "letters blocked"
; result without Windows deciding to bolt on extra UI.
ForceDigitsOnly(ctrl) {
    digitsOnly := RegExReplace(ctrl.Text, "[^0-9]", "")
    if (digitsOnly == ctrl.Text)
        return
    ctrl.Text := digitsOnly
    PostMessage(0xB1, StrLen(digitsOnly), StrLen(digitsOnly), , ctrl.Hwnd)
}

; Parsing the comma-separated groups and tracking which one is active now
; happens inside macro_engine.ahk. This just normalizes the text box and
; writes it to config.ini, which the engine polls for changes.
UpdateKeyGroups(ctrl, *) {
    ForceUppercase(ctrl)
    SaveAllConfig()
}

PressKeyGroup(groupStr) {
    chars := StrSplit(groupStr)
    for c in chars
        Send("{" c " down}")
    for c in chars
        Send("{" c " up}")
}

; NOTE: the Cycle Groups hotkey (G) itself is now registered and handled
; entirely inside macro_engine.ahk, since it's part of the burst-macro
; feature that got split out for speed. This script only owns the
; rebind UI below — it just updates cycleGroupKey and saves it to
; config.ini; the engine process notices the change and re-registers.

; ══════════════════════════════════════════════════════════════
;  HOTKEYS TAB — rebind flow shared by Skill Spammer + Cycle Groups
; ══════════════════════════════════════════════════════════════
StartHotkeyRebind(target) {
    global g_HotkeyRebinding, spammerTriggerBox, cycleKeyBox, TintColor
    if (g_HotkeyRebinding != "")
        return
    g_HotkeyRebinding := target
    box := (target = "spammer") ? spammerTriggerBox : cycleKeyBox
    box.SetFont("s9 Bold c" TintColor, "Consolas")
    box.Value := "..."
    Sleep 200
    BoostModule.DrainKeyState()
    SetTimer(PollForHotkeyRebind, 10)
}

PollForHotkeyRebind() {
    global g_HotkeyRebinding, spammerTriggerKey, cycleGroupKey, TintColor
    global spammerTriggerBox, cycleKeyBox, spammerEnabled

    if (g_HotkeyRebinding = "") {
        SetTimer(PollForHotkeyRebind, 0)
        return
    }

    Loop 254 {
        vk := A_Index
        if !(DllCall("GetAsyncKeyState", "Int", vk, "Short") & 0x8001)
            continue

        box := (g_HotkeyRebinding = "spammer") ? spammerTriggerBox : cycleKeyBox

        if (vk = 0x1B) {  ; Esc cancels, keeps the old key
            SetTimer(PollForHotkeyRebind, 0)
            box.SetFont("s9 Bold c" TintColor, "Consolas")
            box.Value := FriendlyKeyName(g_HotkeyRebinding = "spammer" ? spammerTriggerKey : cycleGroupKey)
            g_HotkeyRebinding := ""
            return
        }
        if (vk = 0x5B || vk = 0x5C)  ; Windows keys stay reserved
            return
        if (vk = 0x01 || vk = 0x02)  ; LButton / RButton stay reserved for clicking the UI
            continue

        keyName := GetKeyName(Format("vk{:X}", vk))
        if (keyName = "")
            return
        SetTimer(PollForHotkeyRebind, 0)

        if (g_HotkeyRebinding = "spammer") {
            spammerTriggerKey := keyName
        } else {
            cycleGroupKey := keyName   ; macro_engine.ahk re-registers itself once this hits config.ini
        }

        UpdateHotkeyHints()
        box.SetFont("s9 Bold c" TintColor, "Consolas")
        box.Value := FriendlyKeyName(keyName)
        g_HotkeyRebinding := ""
        SaveAllConfig()
        return
    }
}

; ══════════════════════════════════════════════════════════════
;  SPAMMER TAB LOGIC
; ══════════════════════════════════════════════════════════════

SpammerCheckboxChanged(ctrl, *) {
    global spammerEnabled
    spammerEnabled := ctrl.Value ? true : false
    SaveAllConfig()
}

UpdateSpammerKey(ctrl, *) {
    global spammerKey
    ForceUppercase(ctrl)
    spammerKey := Trim(ctrl.Text)
    SaveAllConfig()
}

; Polls the trigger key's physical state only — never registered as a real
; Hotkey, so the key (and its native function) keeps working normally whether the
; spammer is enabled, disabled, or the whole script is suspended.
SetTimer(SpammerLoop, 10)

SpammerLoop() {
    global spammerEnabled, spammerKey, spammerTriggerKey, HotkeysSuspended
    if (HotkeysSuspended || !spammerEnabled || spammerKey = "" || spammerTriggerKey = "")
        return
    if (GetKeyState(spammerTriggerKey, "P"))
        PressKeyGroup(spammerKey)
}

; R-spam moved back into AHK (was previously handled by macro_engine.ahk).
; Same polling pattern as SpammerLoop above: never registered as a real
; Hotkey, so R keeps working normally for anything else it's bound to.
; The pixel-watch/burst macro still lives entirely in macro_engine.ahk —
; only the plain "spam R while held" behavior moved here.
SetTimer(RSpamLoop, 10)

RSpamLoop() {
    global rSpamEnabled, HotkeysSuspended
    if (HotkeysSuspended || !rSpamEnabled)
        return
    if (GetKeyState("r", "P"))
        SpamTapR()
}

; Like PressKeyGroup, but for R-spam specifically: releases then immediately
; re-presses, so the key's *resting* synthetic state between ticks is "down"
; (matching the real physical hold) instead of "up". This matters because
; the engine reads R's held-state via GetAsyncKeyState, which reflects
; injected SendInput/Send events globally — not just what AHK itself sees.
; PressKeyGroup's down-then-up order used to leave R reporting "up" for
; ~9 of every 10ms tick, which made the engine's burst-trigger detection
; catch pixel mismatches inconsistently (only during the brief "down" pulse).
SpamTapR() {
    Send("{r up}")
    Send("{r down}")
}

; NOTE: the R-pixel-watch/burst macro (formerly "MacroLoop", SetTimer'd
; at 1ms right here) now runs entirely inside macro_engine.ahk, launched
; hidden by LaunchMacroEngine() below. This script no longer touches the
; pixel or the key groups at runtime (only R-spam, above) — the pixel
; watch and burst logic stay in the engine for speed.

; The cycle-groups hotkey (G) is handled inside macro_engine.ahk too, since
; it feeds directly into the engine's own burst state. The engine writes
; the newly-active group to engine_status.txt each time it cycles; we just
; poll that file and pop a tooltip so pressing G still feels responsive.
global g_LastGroupStatus := ""
SetTimer(PollGroupStatus, 100)

PollGroupStatus() {
    global ConfigFile, g_LastGroupStatus
    statusPath := RegExReplace(ConfigFile, "config\.ini$", "engine_status.txt")
    if !FileExist(statusPath)
        return
    try group := Trim(FileRead(statusPath))
    catch {
        return   ; engine may be mid-write; just try again next poll
    }
    if (group = "" || group = g_LastGroupStatus)
        return
    g_LastGroupStatus := group
    ToolTip("Move: " group)
    SetTimer(() => ToolTip(), -900)
}

; ══════════════════════════════════════════════════════════════
;  EXIT HANDLING
; ══════════════════════════════════════════════════════════════
CleanExit(*) {
    global BoostProcessName, zBoost, clickBoost, g_MacroEnginePID
    zBoost.throttleActive     := false
    clickBoost.throttleActive := false
    SetTimer(zBoost.pollFn, 0)
    SetTimer(clickBoost.pollFn, 0)
    SetTimer(UpdateAllSessionTimers, 0)
    SetTimer(PollAllRoblox, 0)
    SetTimer(SpammerLoop, 0)
    SetTimer(RSpamLoop, 0)
    SetTimer(PollGroupStatus, 0)
    SetTimer(PollCoordPicker, 0)
    ToolTip()
    if (g_MacroEnginePID && ProcessExist(g_MacroEnginePID))
        try ProcessClose(g_MacroEnginePID)
    pid := ProcessExist(BoostProcessName)
    if pid {
        BoostModule.ResumeProcess(pid)
        Sleep 10
        BoostModule.ResumeProcess(pid)
    }
    ExitApp()
}
