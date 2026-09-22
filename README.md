# Hookshot Launcher/Installer

> **A few disclaimers:**
> * AI generated- heavily overseen, but AI generated, so stop here if that bothers you
> * [samuelgr's](https://github.com/samuelgr) launcher is great, but I just have annoying preferences that I'm not going to bother them with
> * Do not slander them because of my decisions, as I know how thorny this all is
> * I have no connection to samuelgr outside of being an enjoyer of their work and a poster of issues
> * DO NOT light up samuel's issue tracker over your experiences unless you can confirm the issue is in the actual Hookshot loader (DLL and EXE combo) and not in "my" launcher code which simply replicates the interfacing the official Hookshot Launcher does against it
> * If you have an issue, please try loading with the [official methods outlined here](https://github.com/samuelgr/Hookshot/wiki/Getting-Started)
> * I have included samuel's license and kept their name attached
> * This is NOT a statement of approval from them or a statement that this is code of their quality
> * The launcher code in particular, however, is far too derivative to deny, and the license should be kept, as it's absolutely a derivative work

---

### Now that we're done with that, still interested?

I have a few bugbears with the original launcher, mainly that it's quite the directory clutterer.

**How to use, what's it do?**
> ```
> Usage:
>   hookshot-launchinst(32/64) [install | uninstall | update] <path-to-exe>
>
> Commands:
>   install     Replaces target executable with a Hookshot-compatible entrypoint
>   uninstall   Restores original executable
>   update      Bring an existing launchinst installation up to date with the current embedded launcher
> ```
> * `install` will determine architecture and copy from its internal resources the actual launcher required for the target executable, handling the rename of the original to the format the launcher expects and renaming the launcher to take its place
> * It also copies icon and version resources from the original executable to make it look good
> * `uninstall` will of course reverse this, and also note any straggler files that you may wish to delete afterward
> * `update` works similarly to `install`, but will refresh an existing installation in-place (good to do if you updated hookshot-launchinst)

**About the executables**
> * Instead of having to determine architecture and place executable(s) manually, the shipped executable is an installer
> * `hookshot-launchinst32.exe` is for 32-bit systems and it only packs the 32-bit launcher
> * `hookshot-launchinst64.exe` covers all your use cases on a modern machine
> * The launcher(s) are contained within the above executables as resources extracted on-demand

**Installing with Scoop**
> * This repo also doubles as a [Scoop](https://scoop.sh) bucket for this project's manifest
> * You can install the bucket like so: `scoop bucket add hookshot-launchinst https://github.com/aro-1790/hookshot-launchinst`
> * Alternately, install it with `bucket_setup.bat` at the root of this repo- it will sparse checkout for you
> * After the bucket's added, you can install launchinst with `scoop install hookshot-launchinst`
> * NEW! You can now install Hookshot from this bucket (autoconfigures %HookshotDir%, too!) `scoop install hookshot`
> * To remove the bucket if you no longer want it: `scoop bucket rm hookshot-launchinst`

**Other details**
> * The launcher itself has a `%HookshotDir%` check- if it doesn't exist, it'll ask you to choose where your root Hookshot directory is (the directory that contains Win32/x64) instead of having to copy over the Hookshot DLL and EXE to every target dir
> * This launcher also automatically creates the Hookshot authorization file (unless one is already present), but cleans up the one it creates when the process is done- unlike the official launcher
> * The launcher also warns about Windows compatibility modes- set on itself, on the real executable (or both!), since HookModules may not behave even though I protect the Hookshot worker from compatibility shims (DLLs are obviously beholden to the shim on its executable parent)
> * You get a choice- you can `Continue` on, but it'll ask you again unless you remove the shims; you can `Cancel`, which will stop it in its tracks; or you can choose "`Don't warn for this executable again`", which will write a persistent marker file `_hks_<exe>.nowarn` so it won't bother you again unless the marker is removed
> * All hookshot-launchinst commands note when a directory-wide `.hookshot` authorization file is present in the target process' folder- it's never created or removed by this tool
> * I use the `_hks_` prefix for the original executable- it's much shorter than the official launcher's `_HookshotLauncher_` prefix, and leaves the original executable's name intact at the end (some programs check their own name, and a suffix trips them up)
