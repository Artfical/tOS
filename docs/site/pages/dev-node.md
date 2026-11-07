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

## npm and packages

`npm` is included with each Node package. It installs from the npm registry over HTTPS:

```
npm --version
mkdir app && cd app
npm init -y
npm install express
node -e "console.log(require('express'))"
```

Packages with native add-ons (C++ code) need the compiler and Python:

```
sudo tpkg install gcc make python3
npm install sqlite3
```

## Scripts and package.json

Add scripts to package.json:

```json
{
  "name": "myapp",
  "version": "1.0.0",
  "scripts": {
    "start": "node index.js",
    "dev": "node --watch index.js",
    "test": "node --test test/*.js"
  }
}
```

Run with `npm start` or `npm run dev`. `npx` runs tools without installing them globally:

```
npx create-react-app my-app
npx typescript --version
```

## ES modules

Node 22 and 24 support ES modules. Use `"type": "module"` in package.json or name the file `index.mjs`:

```js
// index.mjs
import fs from 'node:fs/promises';
import { createServer } from 'node:http';

const server = createServer((req, res) => {
  res.end('hello from Node on tOS');
});
server.listen(8080);
```

## Servers and the network

A server inside a virtual machine is reached through a forwarded port. In QEMU add hostfwd to the network option (guest port 8080 on host port 8080):

```
-nic user,model=e1000,hostfwd=tcp:127.0.0.1:8080-:8080
```

Then open `http://127.0.0.1:8080` on your computer.

```js
import { createServer } from 'node:http';
createServer((req, res) => res.end('hello from tOS')).listen(8080);
```

`fetch`, `dns` and `net` use tOS's network stack (see [Networking](/tos/networking)).

## Files and processes

`fs`, `child_process`, `crypto`, `zlib`, `readline`, `stream`, `path`, `url`, `util`, `events`, `stream`, `assert`, `buffer` all work. `fs.watch` is limited to polling on tOS. `worker_threads` is available but each thread shares the process memory.

## Memory

Node and its packages live in memory (tOS has no swap). Give the virtual machine 2 GB or more. `npm install` of a big project can take time under emulation.

## Related

- [Developing on tOS](/tos/developing) and [Packages](/tos/packages).
