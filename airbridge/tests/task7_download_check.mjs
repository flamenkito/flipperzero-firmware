import { createReadStream } from 'node:fs';
import { unlink } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

for (const path of process.argv.slice(2)) {
  let bytes=0;
  const hash=createHash('sha256');
  try {
    for await (const chunk of createReadStream(path)) {bytes+=chunk.length;hash.update(chunk);}
    assert.equal(bytes,4194369);
    assert.equal(hash.digest('hex'),'1859e54f9399d098d01f22c7d5ade9ba4c264a3c56fa50aa4ba029988a46693a');
    console.log(JSON.stringify({path,bytes,sha256:'1859e54f9399d098d01f22c7d5ade9ba4c264a3c56fa50aa4ba029988a46693a',verified:true}));
  } finally {await unlink(path);}
}
