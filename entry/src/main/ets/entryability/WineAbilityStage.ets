import { AbilityStage, Want } from '@kit.AbilityKit';

export default class WineAbilityStage extends AbilityStage {
  onAcceptWant(want: Want): string {
    // Native toplevel IDs are monotonic for the entire app process, including
    // Wine host restarts. Reusing this key foregrounds the existing UIAbility.
    const id = Number(want.parameters?.toplevelId);
    if (want.abilityName === 'WineWindowAbility' && Number.isSafeInteger(id) && id > 0) {
      return `wine-window:${id}`;
    }
    return 'invalid-wine-window'; // onCreate rejects missing/dead IDs.
  }
}
