import { appTasks } from '@ohos/hvigor-ohos-plugin';
import { verifyWineRuntime } from './scripts/verify_wine_runtime.cjs';

// The ignored guest archive can outlive a checkout change. Hvigor does not
// preserve task arguments in process.argv on every host, so validate whenever
// it loads this project instead of guessing whether this is a package task.
verifyWineRuntime();

export default {
  system: appTasks, /* Built-in plugin of Hvigor. It cannot be modified. */
  plugins: []       /* Custom plugin to extend the functionality of Hvigor. */
}
