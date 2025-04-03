# Posthammer

A pervasive browser-based Rowhammer attack. See the paper (USENIX Security '25).

## Contents

- `bank-functions:` bank addressing information used by the JavaScript scripts
- `js-exploit:` the exploit in JavaScript (late-paper experiments)
- `js-dbg-hugepages:` as above, but relies on huge pages, used for debugging
- `jsshell-130:` not (!) included, download using the link below
- `native-fuzzer:` native fuzzer (early-paper experiments)

## Running it

First, install the dependencies, see below, then:

- `js-exploit:` `make` should work (run it twice, ignore the errors)
- `js-dbg-hugepages:` `make` should work (run it twice, ignore the errors)
- `native-fuzzer:` execute `./main.sh` (run it again if you get an error about `mem.o` missing)

## Dependencies

- For all JavaScript scripts: a Kaby Lake machine (Intel Core i7-7700K CPU)
- For the native fuzzer: a Kaby or Coffee Lake machine (there's a `KABY_LAKE` macro in `pattern.c`)
- The SpiderMonkey shell: `https://ftp.mozilla.org/pub/firefox/releases/130.0/jsshell/`. Place it in a directory called `./jsshell-130` (as above)
- `sudo apt install node-typescript` (for `tsc`)
- `sudo apt install libgsl-dev` (native fuzzer only)
 
## Format

We use Prettier to format `posthammer.ts`: `npm install` should install version 3.3.2 as per `packages.json`.

To format, you do something like:

`npx prettier posthammer.ts`

See also `npx prettier --help`.
 
## Notes

Ignore this error:

`posthammer.ts(1873,20): error TS2304: Cannot find name 'os'.`

(TypeScript doesn't know we're using SpiderMonkey.)

## Debugging

To enable debugging functionality, i.e. inspect physical address information (see `maps.py`), make sure the kernel allows reading from `/dev/mem`. You may have to disable `CONFIG_STRICT_DEVMEM` for this to work.
