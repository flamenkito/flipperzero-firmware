import test from 'node:test';
import { createRequire } from 'node:module';
import { outboundTests } from '../web/protocol-outbound-tests.js';
import { outboundLifecycleTests } from '../web/protocol-outbound-lifecycle-tests.js';
import '../tools/test_web_modules.mjs';
const require = createRequire(import.meta.url);
const vendor = require('../web/vendor/js-sha256-0.11.1.js');
for (const [name, run] of outboundTests(vendor)) test(name, run);
for (const [name, run] of outboundLifecycleTests(vendor)) test(name, run);
