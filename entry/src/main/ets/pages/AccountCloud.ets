import { common } from '@kit.AbilityKit';
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { AppDescriptor, InputProfile } from '../model/AppModels';
import { ProfileSummary, SaveManifest } from '../model/ValueAddedModels';
import { InputProfileService } from '../service/InputProfileService';
import { StoreProduct, ValueAddedService } from '../service/valueadded/ValueAddedService';
import { LocalSaveSnapshotService } from '../service/valueadded/LocalSaveSnapshotService';
import { CloudSaveSlot, CloudSaveVersion } from '../model/CloudSaveModels';
import { CloudSaveService } from '../service/valueadded/CloudSaveService';
import { skinColor } from '../service/SkinColors';

@Entry
@Component
struct AccountCloud {
  @State private supported: boolean = DeviceCapabilityPolicy.supportsValueAddedFeatures();
  @State private busy: boolean = false;
  @State private ready: boolean = false;
  @State private userId: string = '';
  @State private status: string = '';
  @State private membership: string = '未登录';
  @State private products: StoreProduct[] = [];
  @State private profiles: InputProfile[] = [];
  @State private cloudProfiles: ProfileSummary[] = [];
  @State private conflicts: string[] = [];
  @State private games: AppDescriptor[] = [];
  @State private selectedGame: string = '';
  @State private saveDirectory: string = '';
  @State private snapshots: SaveManifest[] = [];
  @State private commerceReady: boolean = false;
  @State private profilesReady: boolean = false;
  @State private cloudSavesReady: boolean = false;
  @State private saveSlots: CloudSaveSlot[] = [];
  @State private selectedSlot: string = '';
  @State private newSlotLabel: string = '';
  @State private cloudQuota: string = '';
  @State private cloudBinding: string = '';
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private alive: boolean = false;

  aboutToAppear(): void {
    this.alive = true;
    // Registered pages are also directly addressable. Guard before any service or SDK initialization.
    if (!this.supported) { router.back(); return; }
    void this.run(async (): Promise<void> => {
      await InputProfileService.getInstance().initialize(this.context);
      this.profiles = InputProfileService.getInstance().getProfiles();
      this.games = LocalSaveSnapshotService.getInstance().eligibleGames();
      this.selectedGame = this.games.length ? this.games[0].id : '';
      if (this.selectedGame) this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, this.selectedGame);
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
      this.cloudSavesReady = service.getCapabilities().cloudSaves;
      this.userId = service.getUserId();
      this.status = this.ready ? '登录后可恢复购买和同步输入方案' : 'NOT_CONFIGURED：账号服务尚未配置';
      if (this.userId) await this.refreshAccount();
    });
  }
  aboutToDisappear(): void { this.alive = false; if (this.supported) CloudSaveService.getInstance().cancel(); }
  private errorMessage(error: Error): string {
    if (error.message === 'NOT_CONFIGURED') return 'NOT_CONFIGURED：服务尚未配置或尚未通过上线检查';
    if (error.message === 'SIGN_IN_REQUIRED') return '登录已过期，请重新登录';
    if (error.message === 'ACCOUNT_CHANGED') return '账号已变更，本次操作已取消';
    if (error.message === 'SAVE_ENGINE_RUNNING') return '请先在现有界面退出 Wine 引擎，再备份或恢复；不会自动停止任何游戏';
    if (error.message === 'SAVE_CHANGED') return '存档仍在变化，本次操作已取消，请关闭游戏后重试';
    if (error.message === 'SAVE_RECOVERY_REQUIRED') return '上次恢复中断，请先点击处理恢复中断';
    if (error.message === 'MEMBERSHIP_REQUIRED') return '云同步需要有效会员，请刷新权益或恢复购买';
    if (error.message === 'SAVE_BINDING_REQUIRED') return '请先把所选云存档位绑定到本机游戏和存档子目录';
    if (error.message === 'SAVE_HISTORY_FULL') return '此存档位已保留3个版本，请先下载保留并删除不需要的云端版本';
    if (error.message === 'SAVE_RECONCILIATION_REQUIRED') return '对象写入结果不明，服务器保留额度等待管理员核对；不会假报已清理';
    if (error.message === 'SAVE_CANCELLED') return '已取消后续传输。请刷新查看已完成的版本和待清理额度';
    return /^[A-Z_]{2,64}$/.test(error.message) ? error.message : '操作未完成，请稍后重试；未确认的购买可通过“恢复购买”重试';
  }
  private async run(action: () => Promise<void>): Promise<void> {
    if (!this.supported || this.busy) return;
    this.busy = true;
    try { await action(); }
    catch (error) {
      if (this.alive) {
        this.status = this.errorMessage(error as Error);
        this.userId = ValueAddedService.getInstance().getUserId();
        if (!this.userId) { this.membership = '未登录'; this.products = []; this.cloudProfiles = []; this.conflicts = []; this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = ''; }
      }
    } finally { if (this.alive) {
      this.busy = false;
      if (CloudSaveService.getInstance().hasLocalCleanupPending()) this.status += '；本地传输临时文件待清理';
    } }
  }
  private async refreshAccount(): Promise<void> {
    const service = ValueAddedService.getInstance();
    this.userId = service.getUserId();
    this.membership = '正在向服务器核验权益';
    const entitlement = await service.entitlement();
    this.membership = entitlement.active && entitlement.expiresAt > entitlement.serverTime ?
      '会员有效至 ' + new Date(entitlement.expiresAt).toLocaleString() :
      entitlement.refreshRequired ? '会员待重新核验，请使用恢复购买' : '当前没有有效会员';
    if (this.commerceReady) this.products = await service.products();
    if (this.profilesReady && entitlement.active) this.cloudProfiles = await service.listCloudProfiles();
    if (this.cloudSavesReady) await this.refreshCloudSaves();
  }
  private login(): void {
    void this.run(async (): Promise<void> => {
      await ValueAddedService.getInstance().signIn();
      await this.refreshAccount();
      this.status = '已登录。身份与权益由服务器核验';
    });
  }
  private logout(): void {
    void this.run(async (): Promise<void> => {
      await ValueAddedService.getInstance().signOut();
      this.userId = ''; this.membership = '未登录'; this.products = []; this.cloudProfiles = []; this.conflicts = [];
      this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = '';
      this.status = '已退出，当前账号的会话和云端状态已清除';
    });
  }
  private buy(product: StoreProduct): void {
    void this.run(async (): Promise<void> => {
      const result = await ValueAddedService.getInstance().purchase(product);
      this.status = result === 'ACK_PENDING' ? '权益核验已完成，商店确认待重试，请使用恢复购买' :
        result === 'FINISHED' ? '购买已核验并确认' : '订单状态：' + result;
      await this.refreshAccount();
    });
  }
  private upload(profile: InputProfile, resolve: boolean = false): void {
    void this.run(async (): Promise<void> => {
      const result = await ValueAddedService.getInstance().uploadProfile(profile, resolve);
      if (result === 'CONFLICT') {
        if (this.conflicts.indexOf(profile.id) < 0) this.conflicts = this.conflicts.concat([profile.id]);
        this.status = '发现云端新版本。已保存本地和云端两份副本，请选择保留哪一份';
      } else {
        this.conflicts = this.conflicts.filter((id: string): boolean => id !== profile.id);
        this.status = '输入方案已上传';
        this.cloudProfiles = await ValueAddedService.getInstance().listCloudProfiles();
      }
    });
  }
  private download(id: string, copy: boolean): void {
    void this.run(async (): Promise<void> => {
      if (!copy) {
        const answer = await promptAction.showDialog({ title: '采用云端输入方案？',
          message: '会先保存本地完整副本，再替换此方案。游戏与其他设置不会上传。',
          buttons: [{ text: '取消', color: '#666666' }, { text: '备份并替换', color: '#356BE8' }] });
        if (answer.index !== 1) return;
      }
      await ValueAddedService.getInstance().downloadProfile(id, copy);
      this.profiles = InputProfileService.getInstance().getProfiles();
      if (!copy) this.conflicts = this.conflicts.filter((value: string): boolean => value !== id);
      this.status = copy ? '已保存为新的输入方案' : '已备份原方案并采用云端版本';
    });
  }
  private backupSave(): void {
    void this.run(async (): Promise<void> => {
      const snapshot = await LocalSaveSnapshotService.getInstance().create(this.context, this.selectedGame, this.saveDirectory.trim());
      this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, this.selectedGame);
      this.status = '本地备份完成：' + snapshot.files.length + ' 个文件，未上传云端';
    });
  }
  private restoreSave(snapshot: SaveManifest): void {
    void this.run(async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '恢复本地存档？',
        message: '将先备份当前 ' + snapshot.selectedPath + '，再恢复选中的快照。请先退出 Wine 引擎。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '备份并恢复', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      await LocalSaveSnapshotService.getInstance().restore(this.context, snapshot.snapshotId, this.selectedGame);
      this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, this.selectedGame);
      this.status = '存档已恢复，恢复前副本已保留';
    });
  }
  private currentSlot(): CloudSaveSlot | undefined {
    return this.saveSlots.find((slot: CloudSaveSlot): boolean => slot.slotId === this.selectedSlot);
  }
  private showBinding(): void {
    const slot = this.currentSlot();
    if (!slot) { this.cloudBinding = ''; return; }
    const binding = CloudSaveService.getInstance().binding(this.context, slot);
    const game = binding ? this.games.find((item: AppDescriptor): boolean => item.id === binding.appId) : undefined;
    this.cloudBinding = binding ? '本机绑定：' + (game?.title ?? binding.appId) + ' / ' + binding.selectedPath : '此设备尚未绑定，下载不会自动猜测游戏目录';
  }
  private async refreshCloudSaves(): Promise<void> {
    const result = await CloudSaveService.getInstance().list();
    this.saveSlots = result.slots;
    if (!this.saveSlots.some((slot: CloudSaveSlot): boolean => slot.slotId === this.selectedSlot)) {
      this.selectedSlot = this.saveSlots.length ? this.saveSlots[0].slotId : '';
    }
    this.cloudQuota = '已用 ' + (result.usedBytes / 1048576).toFixed(1) + ' MiB，上传预留 ' +
      (result.reservedBytes / 1048576).toFixed(1) + ' MiB / 1024 MiB';
    this.showBinding();
  }
  private bindCloudSlot(): void {
    void this.run(async (): Promise<void> => {
      const slot = this.currentSlot(); if (!slot) return;
      const game = this.games.find((item: AppDescriptor): boolean => item.id === this.selectedGame);
      const answer = await promptAction.showDialog({ title: '绑定此设备的存档目录？',
        message: slot.label + ' → ' + (game?.title ?? '') + ' / ' + this.saveDirectory.trim() +
          '。只保存本机映射，不上传路径，也不会立即替换存档。另一台设备须手动选择相同云存档位。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '确认绑定', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      CloudSaveService.getInstance().bind(this.context, slot, this.selectedGame, this.saveDirectory.trim());
      this.showBinding(); this.status = '本机映射已保存';
    });
  }
  private uploadSave(snapshot: SaveManifest): void {
    void this.run(async (): Promise<void> => {
      const slot = this.currentSlot(); if (!slot) return;
      const answer = await promptAction.showDialog({ title: '上传此存档快照？',
        message: '将此快照的游戏存档文件发送到应用配置的私有云存档服务，存入“' + slot.label +
          '”。不会上传游戏目录路径。成功后保留旧版本；冲突时不覆盖另一设备版本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '上传到云端', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      const result = await CloudSaveService.getInstance().upload(this.context, slot, snapshot);
      await this.refreshCloudSaves();
      this.status = result === 'CONFLICT' ? '云端已有新版本，本地快照仍保留。请先下载检查，再明确选择重新上传或新建云存档位' :
        '云存档已校验并发布；其他设备绑定相同云存档位后可下载';
    });
  }
  private downloadSave(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run(async (): Promise<void> => {
      const snapshot = await CloudSaveService.getInstance().download(this.context, slot, version.versionId);
      this.selectedGame = snapshot.appId;
      this.saveDirectory = snapshot.selectedPath;
      this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, this.selectedGame);
      this.status = '云存档已校验为本地快照，游戏文件尚未变动。退出 Wine 引擎后可在下方选择“恢复”';
    });
  }
  private deleteCloudVersion(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run(async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '永久删除此云端版本？',
        message: slot.label + ' · ' + new Date(version.createdAt).toLocaleString() +
          '。删除后云端无法找回；不会删除已下载的本地快照。请先确认保留了需要的副本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '永久删除此版本', color: '#C43838' }] });
      if (answer.index !== 1) return;
      await CloudSaveService.getInstance().deleteVersion(slot, version.versionId);
      await this.refreshCloudSaves(); this.status = '此云端版本已删除，本地文件未变更';
    });
  }
  @Builder
  private SectionTitle(title: string, detail: string) {
    Text(title).fontSize(21).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary')).width('100%')
    Text(detail).fontSize(13).fontColor(skinColor('text_secondary')).width('100%')
  }
  build() {
    if (this.supported) {
      Column({ space: 16 }) {
        Row({ space: 16 }) {
          Button('返回').onClick((): void => router.back())
          Text('账号与云服务').fontSize(25).fontWeight(FontWeight.Bold).layoutWeight(1)
          if (this.busy) { LoadingProgress().width(28).height(28) }
        }.width('100%')
        Scroll() {
          Column({ space: 20 }) {
            Column({ space: 12 }) {
              this.SectionTitle('华为账号', '使用华为授权码登录。授权码仅发送到应用配置的服务端换取短期会话，设备不保存密码。')
              Text(this.userId ? '账号：' + this.userId : '未登录').fontSize(14).width('100%')
              Text(this.membership).fontSize(14).width('100%')
              if (!this.userId) {
                Button('使用华为账号登录').enabled(this.ready && !this.busy).onClick((): void => this.login())
              } else {
                Row({ space: 12 }) {
                  Button('刷新权益').enabled(!this.busy).onClick((): void => {
                    void this.run(async (): Promise<void> => { await this.refreshAccount(); });
                  })
                  Button('退出登录').enabled(!this.busy).onClick((): void => this.logout())
                }
              }
            }.width('100%').padding(20).backgroundColor(skinColor('search_background')).borderRadius(18)
            Text(this.status).fontSize(14).fontColor(skinColor('text_secondary')).width('100%').id('account-cloud-status')
            Column({ space: 12 }) {
              this.SectionTitle('会员与支持开发', '会员用于云同步。支持开发为独立消耗型商品，不增加会员时长。付款前请核对华为收银台的价格、期限和续费条款。')
              if (!this.commerceReady) { Text('NOT_CONFIGURED：购买尚未开放').fontSize(14).width('100%') }
              ForEach(this.products, (product: StoreProduct) => {
                Button(product.title + ' · ' + product.price).width('100%').enabled(!this.busy)
                  .onClick((): void => this.buy(product))
              }, (product: StoreProduct): string => product.id)
              Button('恢复购买 / 重试确认').enabled(this.commerceReady && !!this.userId && !this.busy)
                .onClick((): void => {
                  void this.run(async (): Promise<void> => {
                    const result = await ValueAddedService.getInstance().restorePurchases();
                    await this.refreshAccount();
                    this.status = result === 'ACK_PENDING' ? '部分订单确认仍待重试' : '恢复购买核验完成';
                  });
                })
            }.width('100%').padding(20).backgroundColor(skinColor('search_background')).borderRadius(18)
            Column({ space: 12 }) {
              this.SectionTitle('输入方案云同步', '只同步完整 v6 按键布局与手柄映射。平板与桌面各自保存版本；不会上传游戏路径、Steam 凭据或其他设置。')
              if (!this.profilesReady) { Text('NOT_CONFIGURED：配置云同步尚未开放').fontSize(14).width('100%') }
              ForEach(this.profiles, (profile: InputProfile) => {
                Column({ space: 8 }) {
                  Row({ space: 12 }) {
                    Text(profile.name).layoutWeight(1)
                    Button('上传').enabled(this.profilesReady && !!this.userId && !this.busy)
                      .onClick((): void => this.upload(profile))
                  }.width('100%')
                  if (this.conflicts.indexOf(profile.id) >= 0) {
                    Row({ space: 8 }) {
                      Button('保留本地').enabled(!this.busy).onClick((): void => this.upload(profile, true))
                      Button('采用云端').enabled(!this.busy).onClick((): void => this.download(profile.id, false))
                      Button('另存云端副本').enabled(!this.busy).onClick((): void => this.download(profile.id, true))
                    }
                  }
                }.width('100%')
              }, (profile: InputProfile): string => profile.id)
              ForEach(this.cloudProfiles, (profile: ProfileSummary) => {
                Row({ space: 12 }) {
                  Text(profile.name + ' · 云端 v' + profile.revision).layoutWeight(1)
                  Button('下载为副本').enabled(!this.busy).onClick((): void => this.download(profile.variant, true))
                }.width('100%')
              }, (profile: ProfileSummary): string => profile.variant)
            }.width('100%').padding(20).backgroundColor(skinColor('search_background')).borderRadius(18)
            Column({ space: 12 }) {
              this.SectionTitle('本地存档快照', '首版仅备份手选游戏安装目录下的一个存档子目录。请填写相对目录，如 save 或 Saved Games。Steam、整个前缀和注册表不在此范围。需先退出 Wine 引擎，不会自动停止任何游戏。')
              if (this.games.length > 0) {
                Select(this.games.map((game: AppDescriptor): SelectOption => { return { value: game.title }; }))
                  .selected(this.games.findIndex((game: AppDescriptor): boolean => game.id === this.selectedGame)).enabled(!this.busy)
                  .onSelect((index: number): void => {
                    this.selectedGame = this.games[index].id;
                    this.saveDirectory = '';
                    this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, this.selectedGame);
                  })
                TextInput({ placeholder: '游戏安装目录内的存档子目录', text: this.saveDirectory })
                  .enabled(!this.busy).onChange((value: string): void => { this.saveDirectory = value; })
                Button('创建本地快照').enabled(!this.busy && !!this.selectedGame && !!this.saveDirectory.trim())
                  .onClick((): void => this.backupSave())
                ForEach(this.snapshots, (snapshot: SaveManifest) => {
                  Row({ space: 12 }) {
                    Text(snapshot.selectedPath + ' · ' + new Date(snapshot.createdAt).toLocaleString() +
                      ' · ' + snapshot.files.length + ' 文件').layoutWeight(1)
                    Button('恢复').enabled(!this.busy).onClick((): void => this.restoreSave(snapshot))
                    Button('上传云端').enabled(this.cloudSavesReady && !!this.userId && !!this.selectedSlot && !this.busy)
                      .onClick((): void => this.uploadSave(snapshot))
                  }.width('100%')
                }, (snapshot: SaveManifest): string => snapshot.snapshotId)
                Button('处理恢复中断').enabled(!this.busy).onClick((): void => {
                  void this.run(async (): Promise<void> => {
                    LocalSaveSnapshotService.getInstance().recoverInterrupted(this.context);
                    this.status = '恢复检查完成，原有文件均保留';
                  });
                })
              } else { Text('没有符合条件的本地游戏。请先手动导入游戏。').fontSize(14).width('100%') }
            }.width('100%').padding(20).backgroundColor(skinColor('search_background')).borderRadius(18)
            Column({ space: 12 }) {
              this.SectionTitle('云存档', '手动绑定同一个云存档位后跨设备下载。每位最多3个版本、单快照256 MiB；下载先生成本地快照，再单独确认恢复，不会自动替换游戏文件。')
              if (!this.cloudSavesReady) { Text('NOT_CONFIGURED：云存档未配置或尚未通过存储验收').fontSize(14).width('100%') }
              Text(this.cloudQuota).fontSize(14).width('100%')
              TextInput({ placeholder: '新云存档位名称，如 游戏名·主存档', text: this.newSlotLabel })
                .enabled(this.cloudSavesReady && !!this.userId && !this.busy)
                .onChange((value: string): void => { this.newSlotLabel = value; })
              Row({ space: 8 }) {
                Button('新建存档位').enabled(this.cloudSavesReady && !!this.userId && !!this.newSlotLabel.trim() && !this.busy)
                  .onClick((): void => { void this.run(async (): Promise<void> => {
                    const slot = await CloudSaveService.getInstance().createSlot(this.newSlotLabel);
                    this.selectedSlot = slot.slotId; this.newSlotLabel = ''; await this.refreshCloudSaves();
                  }); })
                Button('刷新云端').enabled(this.cloudSavesReady && !!this.userId && !this.busy)
                  .onClick((): void => { void this.run(async (): Promise<void> => { await this.refreshCloudSaves(); }); })
                Button('取消传输').enabled(this.busy && this.cloudSavesReady).onClick((): void => {
                  CloudSaveService.getInstance().cancel(); this.status = '已请求取消，将停止后续块并尝试清理预留';
                })
              }
              if (this.saveSlots.length > 0) {
                Select(this.saveSlots.map((slot: CloudSaveSlot): SelectOption => { return { value: slot.label }; }))
                  .selected(this.saveSlots.findIndex((slot: CloudSaveSlot): boolean => slot.slotId === this.selectedSlot)).enabled(!this.busy)
                  .onSelect((index: number): void => { this.selectedSlot = this.saveSlots[index].slotId; this.showBinding(); })
                Text(this.cloudBinding).fontSize(14).width('100%')
                Button('绑定到上方游戏和存档目录').enabled(!!this.selectedGame && !!this.saveDirectory.trim() && !this.busy)
                  .onClick((): void => this.bindCloudSlot())
                ForEach(this.saveSlots, (slot: CloudSaveSlot) => {
                  if (slot.slotId === this.selectedSlot) {
                    ForEach(slot.versions, (version: CloudSaveVersion) => {
                      Row({ space: 8 }) {
                        Text(new Date(version.createdAt).toLocaleString() + ' · ' + (version.totalBytes / 1048576).toFixed(1) + ' MiB').layoutWeight(1)
                        Button('下载为快照').enabled(!this.busy).onClick((): void => this.downloadSave(slot, version))
                        Button('删除云版本').enabled(!this.busy).onClick((): void => this.deleteCloudVersion(slot, version))
                      }.width('100%')
                    }, (version: CloudSaveVersion): string => version.versionId)
                  }
                }, (slot: CloudSaveSlot): string => slot.slotId)
              }
              Button('清理本地传输临时文件').enabled(!this.busy).onClick((): void => {
                void this.run(async (): Promise<void> => {
                  CloudSaveService.getInstance().cleanupLocal(this.context); this.status = '本地传输临时文件已清理，存档和快照未变更';
                });
              })
              Button('清理过期或已取消的上传').enabled(this.cloudSavesReady && !!this.userId && !this.busy)
                .onClick((): void => { void this.run(async (): Promise<void> => {
                  const state = await CloudSaveService.getInstance().cleanup(); await this.refreshCloudSaves();
                  this.status = state === 'CLEANED' ? '可安全清理的上传已处理' : state === 'RECONCILIATION_REQUIRED' ?
                    '部分对象写入结果不明，需要管理员核对；预留额度继续保留' : '仍有传输或等待安全清理的预留，请稍后重试';
                }); })
            }.width('100%').padding(20).backgroundColor(skinColor('search_background')).borderRadius(18)
            Text('游戏和本地功能仍可免费使用。云存档只在你确认上传后传输，不会后台扫描游戏目录。')
              .fontSize(13).fontColor(skinColor('text_secondary')).width('100%')
          }.width('100%').constraintSize({ maxWidth: 900 }).padding({ bottom: 32 })
        }.layoutWeight(1).width('100%').scrollBar(BarState.Auto)
      }.padding(24).width('100%').height('100%').backgroundColor(skinColor('page_background'))
    } else { Column() { }.width('100%').height('100%') }
  }
}
