import { test } from 'node:test';
import { createRequire } from 'node:module';
import { itemReceiverTests } from '../web/protocol-item-receiver-tests.js';
import { itemReceiverLifecycleTests } from '../web/protocol-item-receiver-lifecycle-tests.js';
import { itemReceiverAuthTests } from '../web/protocol-item-receiver-auth-tests.js';
import { itemReceiverGenerationTests } from '../web/protocol-item-receiver-generation-tests.js';
import { incompatibilityTests } from '../web/protocol-incompatibility-tests.js';

const vendor = createRequire(import.meta.url)('../web/vendor/js-sha256-0.11.1.js');
for (const [name, run] of itemReceiverTests(vendor)) test(name, run);
for (const [name, run] of itemReceiverLifecycleTests(vendor)) test(name, run);
for (const [name, run] of itemReceiverAuthTests(vendor)) test(name, run);
for (const [name, run] of itemReceiverGenerationTests(vendor)) test(name, run);
for (const [name, run] of incompatibilityTests(vendor)) test(name, run);
