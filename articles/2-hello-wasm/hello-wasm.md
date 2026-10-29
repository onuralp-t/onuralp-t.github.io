# Hello WASM, Hello SQLite, Hello everyone!
This article will describe the workings of [this repo of mine](https://github.com/onuralp-t/hello-sqlite-wasm), and what is necessary to serve a static website on Github Pages that uses SQLite's WASM build; moving the high-level explanation from code comments to, hopefully, where it makes more sense.

# Before we even start: CORS and WASM
To load in WASM binaries, you need to be in a cross origin isolated context. Usually this is not a problem, but because I use Github Pages to host the website in my repo, and because Github doesn't give users enough control to set these kinds of headers, we need to fix this CORS issue in another way.

We need COOP and COEP HTTP headers set like:
```
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Opener-Policy: same-origin
```
Luckily, we're not the first one to encounter this problem, and there's this hack: https://github.com/gzuidhof/coi-serviceworker , thank you gzuidhof.

# What's going on in the main thread in index.html?
At the top, we have our message_types: the types of messages that we can send between our worker and main thread.
```js
const message_types = Object.freeze({
    MAIN_ENTER_TEXT: 0,
    MAIN_CLEAR_DB: 1,
    WORKER_DB_CONTENTS: 2,
});
```
Currently, I manually define this struct in both main and the worker, which is kind of a stupid implementation, but I didn't want to create another `common.js` to include in both main and worker files, to keep things simple.

After creating the worker, we define when the main thread will send which messages to the worker thread, and with what payload. 
```js
const worker = new Worker('my-sqlite-worker.js');

user_input.addEventListener('keydown', e => {
    if (e.key === 'Enter' && user_input.value) {
    worker.postMessage({type: message_types.MAIN_ENTER_TEXT, payload: user_input.value});
    user_input.value = '';
    }
});

clear_db_button.addEventListener('click', e => {
    worker.postMessage({type: message_types.MAIN_CLEAR_DB, payload: 0});
})
```

Then, we define how the main thread will handle incomming messages from the worker thread.
```js
worker.onmessage = e => {
    switch(e.data.type)
    {
        case message_types.WORKER_DB_CONTENTS:
        {
            let htmlstr = "";
            for (row of e.data.payload) {
                htmlstr += `${row}<br>`;
            }
            db_contents.innerHTML = htmlstr;
        } break;
    }
};
```
# What's going on in the worker thread?

In `my-sqlite-worker.js`, we start by including the JavaScript release of the SQLite library, and create the same message struct I talked about earlier:
```js
importScripts("sqlite3.js");

const message_types = Object.freeze({
    MAIN_ENTER_TEXT: 0,
    MAIN_CLEAR_DB: 1,
    WORKER_DB_CONTENTS: 2,
});
```

To initialize the database, we do:
```js
sqlite3InitModule().then(sqlite3 => {
    const db = new sqlite3.oo1.OpfsDb('/test.db');
    // ...
```
- There are a [couple of different ways](https://sqlite.org/wasm/doc/trunk/persistence.md) of achieving persistence when using sqlite-wasm. To write to disk, this little program uses the [OPFS](https://developer.mozilla.org/en-US/docs/Web/API/File_System_API/Origin_private_file_system) (see: O stands for Origin).

We create the table, and then a helper function to query everything from the table and send it to the main thread through a message, and then call the helper to send the initial state to main:
```js
db.exec('CREATE TABLE IF NOT EXISTS entries (text TEXT)');
const send_db_to_main = () => {
const db_contents = db.exec('SELECT * FROM entries', { returnValue: 'resultRows' });
    self.postMessage({type: message_types.WORKER_DB_CONTENTS, payload: db_contents});
};
    
send_db_to_main();
```

We then implement how we're gonna handle different messages that can come from main:
```js
self.onmessage = e => {
    switch(e.data.type)
	{
    case message_types.MAIN_ENTER_TEXT:
        {
            db.exec({ sql: 'INSERT INTO entries (text) VALUES (?)', bind: [e.data.payload] });
            const rows = db.exec('SELECT * FROM entries', { returnValue: 'resultRows' });
            send_db_to_main();
        } break;

        case message_types.MAIN_CLEAR_DB:
        {
            db.exec({ sql: 'DELETE FROM entries' });
            send_db_to_main();
        } break;
    } // switch(e.data.type)
}; // self.onmessage
}); // sqlite3InitModule().then
```
