title: Node.js
lede: Node.js 22 and 24, and an old Node 8, run on tOS with npm. This page shows how to install and use them.
---
## Install

There are three Node packages. Each brings the commands `node`, `npm` and `npx`, so install **one** (tpkg warns you if another one already provides them):

| Package | Version | Notes |
|---|---|---|
| `node24` | 24.x | The newest |
| `node22` | 22.x | Long-term support |
| `node` | 8.17 | An old version, for programs that need it |

```
sudo tpkg install node24
node -v
node -e "console.log(6 * 7)"
```

## A first program

Save this as `hello.js`:

```
const os = require('os');
console.log('hello from Node on', os.platform(), process.arch);
```

and run it:

```
node hello.js
```

## npm

`npm` installs packages from the npm registry:

```
npm --version
mkdir app && cd app
npm init -y
npm install left-pad
node -e "console.log(require('left-pad')('x', 5))"
```

Packages with native add-ons need the compiler: `sudo tpkg install gcc make python3` first.

## A project with package.json

```
npm init -y
```

makes a `package.json`. Add a script and run it with npm:

```
{
  "name": "app",
  "version": "1.0.0",
  "scripts": { "start": "node index.js" }
}
```

```
npm start
npx <tool>        # runs a tool from the registry or from node_modules/.bin
```

ES modules work in the current versions: use `"type": "module"` in `package.json` or name the file `index.mjs`, then `import fs from 'node:fs'`.

## Servers

```
node -e "require('http').createServer((q, r) => r.end('hello\n')).listen(8080)"
```

starts a web server on port 8080, using tOS's sockets. `http`, `https` and `net` use the network stack (see [Networking](/tos/networking)).

### Reaching a server from your computer

A server inside a virtual machine is reached through a forwarded port. In QEMU add `hostfwd` to the network option (here guest port 8080 on host port 8080):

```
-nic user,model=e1000,hostfwd=tcp:127.0.0.1:8080-:8080
```

then open `http://127.0.0.1:8080` on your computer.

## Files and processes

Most of Node's standard modules work, among them `fs`, `child_process`, `crypto`, `zlib`, `readline` and `stream`.

## Memory

Node and its packages are big, and everything lives in memory: give a virtual machine 2 GB or more. `npm install` of a big project can take time under emulation.

## Related

- [Developing on tOS](/tos/developing) and [Packages](/tos/packages).
