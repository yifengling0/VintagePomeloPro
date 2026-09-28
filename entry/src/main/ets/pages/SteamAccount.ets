import { window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { router } from '@kit.ArkUI';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { applyStatusOnlyImmersiveShell } from '../common/StatusOnlyImmersiveShell';
import { SteamService, SteamUiState } from '../steam/SteamService';

/**
 * Steam 账号页：账号登录 / Steam Guard 令牌 / 游戏库 三段式。
 * 入口在主页顶栏头像按钮（弹窗），复杂流程在本页完成。
 */
@Entry
@Component
struct SteamAccount {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  @State private state: SteamUiState = SteamUiState.IDLE;
  @State private message: string = '';
  @State private accountName: string = '';
  @State private steamId64: string = '';
  @State private personaName: string = '';
  @State private avatarUrl: string = '';
  @State private inputAccount: string = '';
  @State private inputPassword: string = '';
  @State private inputCode: string = '';
  @State private probeResult: string = '检测 Steam 连接中…';
  @State private probeOk: boolean = false;
  @State private guardCode: string = '';
  @State private guardCodeSeconds: number = 0;
  @State private hasSharedSecret: boolean = false;
  @State private secretInput: string = '';
  @State private importing: boolean = false;
  @State private tokenMessage: string = '';
  private codeTimer: number = -1;
  private confirmTimer: number = -1;

  aboutToAppear(): void {
    this.applyWindowChrome();
    this.startProbe();
    SteamService.getInstance().initialize(this.context).then((): void => {
      this.refreshFromService();
      SteamService.getInstance().subscribe('SteamAccountPage',
        (state: SteamUiState, message: string): void => {
          this.state = state;
          this.message = message;
          this.refreshFromService();
          this.syncTimers();
        });
      if (SteamService.getInstance().steamId64.length > 0) {
        SteamService.getInstance().restoreSession().then((ok: boolean): void => {
          if (!ok) {
            this.message = '会话已过期，请重新登录';
          }
          this.refreshFromService();
          this.refreshGuardSecretFlag();
        });
      } else {
        this.refreshGuardSecretFlag();
      }
    });
  }

  aboutToDisappear(): void {
    SteamService.getInstance().unsubscribe('SteamAccountPage');
    this.stopTimers();
  }

  /** 登录页同样强制横板左右旋转（与设置页同策略）。 */
  private applyWindowChrome(): void {
    window.getLastWindow(this.context).then(async (pageWindow: window.Window): Promise<void> => {
      DeviceCapabilityPolicy.applyLauncherLandscapeRotation(pageWindow, 'SteamAccount');
      await applyStatusOnlyImmersiveShell(pageWindow);
    }).catch((_e: Error): void => {
    });
  }

  private refreshFromService(): void {
    const service = SteamService.getInstance();
    this.accountName = service.accountName;
    this.steamId64 = service.steamId64;
    this.personaName = service.personaName;
    this.avatarUrl = service.avatarUrl;
  }

  private startProbe(): void {
    SteamService.getInstance().probeNetwork().then((error: string | null): void => {
      this.probeOk = error === null;
      this.probeResult = error === null ? 'Steam 连接正常' : error;
    });
  }

  private refreshGuardSecretFlag(): void {
    if (this.steamId64.length === 0) {
      this.hasSharedSecret = false;
      return;
    }
    SteamService.getInstance().currentGuardCode().then((code: string): void => {
      this.hasSharedSecret = code.length > 0;
      if (this.hasSharedSecret) {
        this.startGuardTimer();
      }
    });
  }

  /** W3: 文件选择器选 maFile → 读文本 → Service 解析入库。 */
  private pickMaFile(): void {
    const docPicker = new picker.DocumentViewPicker(this.context);
    const options = new picker.DocumentSelectOptions();
    this.importing = true;
    docPicker.select(options).then((uris: Array<string>): void => {
      this.importing = false;
      if (uris.length === 0 || !uris[0]) {
        return;
      }
      const text = this.readPickedText(uris[0]);
      if (text.length === 0) {
        this.tokenMessage = '读取文件失败（文件不可读或非文本）';
        return;
      }
      SteamService.getInstance().importMaFile(text).then((report: string): void => {
        this.tokenMessage = report;
        this.refreshGuardSecretFlag();
      });
    }).catch((_e: Object): void => {
      this.importing = false;
    });
  }

  private readPickedText(uri: string): string {
    let fd: number = -1;
    try {
      const file = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      fd = file.fd;
      const stat = fs.statSync(fd);
      const buf = new ArrayBuffer(stat.size);
      fs.readSync(fd, buf);
      const bytes = new Uint8Array(buf);
      return util.TextDecoder.create('utf-8', { fatal: false }).decodeWithStream(bytes, { stream: false });
    } catch (_) {
      return '';
    } finally {
      if (fd >= 0) {
        try {
          fs.closeSync(fd);
        } catch (_) {
        }
      }
    }
  }

  private syncTimers(): void {
    if (this.state === SteamUiState.NEEDS_CONFIRM) {
      this.startConfirmTimer();
    } else {
      this.stopConfirmTimer();
    }
  }

  private startGuardTimer(): void {
    if (this.codeTimer >= 0) return;
    const tick = (): void => {
      SteamService.getInstance().currentGuardCode().then((code: string): void => {
        this.guardCode = code;
        const epoch = Math.floor(Date.now() / 1000);
        this.guardCodeSeconds = 30 - (epoch % 30);
      });
    };
    tick();
    this.codeTimer = setInterval(tick, 1000);
  }

  private stopGuardTimer(): void {
    if (this.codeTimer >= 0) {
      clearInterval(this.codeTimer);
      this.codeTimer = -1;
    }
  }

  private startConfirmTimer(): void {
    if (this.confirmTimer >= 0) return;
    this.confirmTimer = setInterval((): void => {
      SteamService.getInstance().pollOnce();
    }, 2500);
  }

  private stopConfirmTimer(): void {
    if (this.confirmTimer >= 0) {
      clearInterval(this.confirmTimer);
      this.confirmTimer = -1;
    }
  }

  private stopTimers(): void {
    this.stopGuardTimer();
    this.stopConfirmTimer();
  }

  @Builder
  private TopBar() {
    Row() {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(22)
          .fontColor([$r('app.color.text_primary')])
      }
      .width(32).height(32)
      .backgroundColor($r('app.color.search_background'))
      .onClick((): void => { router.back(); })
      .id('steam-back-button')

      Text('Steam 账号').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary')).margin({ left: 10 })
        .layoutWeight(1)
    }
    .width('100%').height(40)
  }

  @Builder
  private ProfileCard() {
    Column({ space: 12 }) {
      Row({ space: 12 }) {
        if (this.avatarUrl.length > 0) {
          Image(this.avatarUrl)
            .width(56).height(56).borderRadius(16)
            .objectFit(ImageFit.Cover)
            .draggable(false)
        } else {
          Stack() {
            SymbolGlyph($r('sys.symbol.person')).fontSize(30)
              .fontColor([$r('app.color.text_secondary')])
          }
          .width(56).height(56).borderRadius(16)
          .backgroundColor($r('app.color.selected_background'))
        }
        Column({ space: 3 }) {
          Text(this.personaName.length > 0 ? this.personaName : this.accountName)
            .fontSize(17).fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary')).width('100%')
          if (this.accountName.length > 0) {
            Text(this.accountName).fontSize(12)
              .fontColor($r('app.color.text_secondary')).width('100%')
          }
          if (this.steamId64.length > 0) {
            Text('SteamID ' + this.steamId64).fontSize(11)
              .fontColor($r('app.color.text_secondary')).width('100%')
          }
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.width('100%')

      Button('登出').width('100%').height(40).borderRadius(12)
        .backgroundColor($r('app.color.search_background'))
        .fontColor($r('app.color.text_primary'))
        .onClick((): void => {
          SteamService.getInstance().signOut();
          this.stopGuardTimer();
          this.hasSharedSecret = false;
        })
        .id('steam-logout')
    }
    .width('100%').padding(14)
    .backgroundColor($r('app.color.search_background'))
    .borderRadius(14)
  }

  @Builder
  private TokenCard() {
    Column({ space: 10 }) {
      Text('Steam 手机令牌').fontSize(13).fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary')).width('100%')
      if (this.hasSharedSecret) {
        Stack() {
          Text(this.guardCode.length > 0 ? this.guardCode : '-----')
            .fontSize(34).fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.accent'))
            .letterSpacing(6)
        }
        .width('100%').height(72).borderRadius(14)
        .backgroundColor($r('app.color.selected_background'))
        .onClick((): void => {
          if (this.guardCode.length > 0) {
            const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.guardCode);
            pasteboard.getSystemPasteboard().setPasteData(data);
            this.message = '令牌码已复制';
          }
        })
        .id('steam-totp-code')
        Progress({ value: this.guardCodeSeconds, total: 30, type: ProgressType.Linear })
          .width('100%').color($r('app.color.accent'))
        Text('与 Steam 手机 App 一致，30 秒刷新；点击复制')
          .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
      } else {
        Text('用 Steam 手机令牌的 maFile 导入（推荐），或手动粘贴 shared_secret（Base64）。')
          .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
        Button(this.importing ? '读取中…' : '导入 maFile 文件')
          .width('100%').height(38).borderRadius(10)
          .backgroundColor($r('app.color.selected_background'))
          .fontColor($r('app.color.text_primary'))
          .enabled(!this.importing)
          .onClick((): void => { this.pickMaFile(); })
          .id('steam-mafile-import')
        if (this.tokenMessage.length > 0) {
          Text(this.tokenMessage).fontSize(11)
            .fontColor($r('app.color.text_secondary')).width('100%')
            .id('steam-mafile-message')
        }
        TextInput({ placeholder: 'shared_secret（Base64）', text: this.secretInput })
          .fontSize(12).height(38).borderRadius(10)
          .backgroundColor($r('app.color.card_background'))
          .onChange((value: string): void => { this.secretInput = value; })
          .id('steam-secret-input')
        Button(this.importing ? '保存中…' : '保存密钥并启用令牌')
          .width('100%').height(38).borderRadius(10)
          .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
          .enabled(!this.importing && this.secretInput.trim().length > 0)
          .onClick((): void => {
            this.importing = true;
            SteamService.getInstance().saveSharedSecret(this.secretInput).then((): void => {
              this.importing = false;
              this.secretInput = '';
              this.refreshGuardSecretFlag();
            });
          })
          .id('steam-secret-save')
      }
    }
    .width('100%').padding(14)
    .backgroundColor($r('app.color.search_background'))
    .borderRadius(14)
  }

  build() {
    Column() {
      this.TopBar()
      Scroll() {
        Column({ space: 12 }) {
          Row({ space: 8 }) {
            Circle({ width: 8, height: 8 })
              .fill(this.probeOk ? '#4ADE80' : '#F87171')
            Text(this.probeResult).fontSize(12)
              .fontColor($r('app.color.text_secondary')).layoutWeight(1)
          }
          .width('100%').padding(10)
          .backgroundColor($r('app.color.search_background'))
          .borderRadius(10)
          .id('steam-probe')

          if (this.state === SteamUiState.LOGGED_IN && this.steamId64.length > 0) {
            this.ProfileCard()
            this.TokenCard()
          } else {
            Column({ space: 10 }) {
              Text('登录 Steam').fontSize(13).fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary')).width('100%')

              if (this.state === SteamUiState.NEEDS_EMAIL_CODE ||
                this.state === SteamUiState.NEEDS_GUARD_CODE) {
                Text(this.state === SteamUiState.NEEDS_GUARD_CODE
                  ? '输入手机令牌 App 上的 5 位验证码' : '输入邮箱收到的验证码')
                  .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
                TextInput({ placeholder: '验证码', text: this.inputCode })
                  .fontSize(14).height(44).borderRadius(12)
                  .backgroundColor($r('app.color.card_background'))
                  .onChange((value: string): void => { this.inputCode = value; })
                  .id('steam-code-input')
                Button('提交验证码').width('100%').height(44).borderRadius(12)
                  .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
                  .enabled(this.inputCode.trim().length > 0)
                  .onClick((): void => {
                    SteamService.getInstance().submitCode(this.inputCode);
                  })
                  .id('steam-code-submit')
              } else if (this.state === SteamUiState.NEEDS_CONFIRM) {
                LoadingProgress().width(36).height(36).color($r('app.color.accent'))
                Text('请在手机 Steam 上确认本次登录…')
                  .fontSize(13).fontColor($r('app.color.text_primary'))
              } else {
                TextInput({ placeholder: 'Steam 账号名', text: this.inputAccount })
                  .fontSize(14).height(44).borderRadius(12)
                  .backgroundColor($r('app.color.card_background'))
                  .onChange((value: string): void => { this.inputAccount = value; })
                  .id('steam-login-account')
                TextInput({ placeholder: '密码', text: this.inputPassword })
                  .type(InputType.Password)
                  .fontSize(14).height(44).borderRadius(12)
                  .backgroundColor($r('app.color.card_background'))
                  .onChange((value: string): void => { this.inputPassword = value; })
                  .id('steam-login-password')
                Button(this.state === SteamUiState.WORKING ? '登录中…' : '登录')
                  .width('100%').height(44).borderRadius(12)
                  .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
                  .enabled(this.state !== SteamUiState.WORKING)
                  .onClick((): void => {
                    SteamService.getInstance().signIn(this.inputAccount, this.inputPassword);
                  })
                  .id('steam-login-button')
                Text('建议使用小号测试；新设备登录可能触发 Steam 邮箱验证，频繁验证码属 Steam 风控，与客户端无关。')
                  .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
              }

              if (this.message.length > 0) {
                Text(this.message).fontSize(12)
                  .fontColor(this.state === SteamUiState.ERROR ? '#F87171' : $r('app.color.text_secondary'))
                  .width('100%').id('steam-message')
              }
              if (this.state === SteamUiState.WORKING) {
                LoadingProgress().width(28).height(28).color($r('app.color.accent'))
              }
            }
            .width('100%').padding(14)
            .backgroundColor($r('app.color.search_background'))
            .borderRadius(14)
          }
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 8, bottom: 32 })
        .constraintSize({ minHeight: '100%' })
      }
      .layoutWeight(1)
      .width('100%')
      .scrollBar(BarState.Off)
      .id('steam-account-scroll')
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_background'))
  }
}
