import { promptAction } from '@kit.ArkUI';
import { HostWindowTask, HostWindowTasks, HostTaskListener } from '../service/HostWindowTasks';
import { InputDispatcher } from '../service/InputDispatcher';

// Each host surface carries the same recovery entry, including fullscreen ones.
@Component
export struct HostTaskSwitcher {
  @State private tasks: HostWindowTask[] = [];
  @State private expanded: boolean = false;
  @State private switching: boolean = false;
  private listener: HostTaskListener = (tasks: HostWindowTask[]): void => { this.tasks = tasks; };

  aboutToAppear(): void { HostWindowTasks.getInstance().subscribe(this.listener); }
  aboutToDisappear(): void { HostWindowTasks.getInstance().unsubscribe(this.listener); }

  private label(task: HostWindowTask): string {
    return task.source === 'steam-linux' ? 'Steam（Linux ARM64）' : task.title;
  }

  private async switchTo(task: HostWindowTask): Promise<void> {
    if (this.switching) return;
    this.switching = true;
    try {
      const restored = await HostWindowTasks.getInstance().activate(task.key);
      if (restored) this.expanded = false;
      else promptAction.showToast({ message: '窗口暂时无法恢复，请稍后重试' });
    } catch (_) {
      promptAction.showToast({ message: '恢复窗口失败，请重试' });
    } finally { this.switching = false; }
  }

  build() {
    if (this.tasks.length > 0) {
      Column({ space: 8 }) {
        Button(`运行任务 · ${this.tasks.length}`)
          .id('host-task-toggle')
          .height(36).fontSize(13).fontColor('#FFFFFF')
          .backgroundColor('#DD24354B')
          .onClick((): void => {
            InputDispatcher.getInstance().releaseAll('host-task-panel');
            this.expanded = !this.expanded;
          })
        if (this.expanded) {
          Column({ space: 6 }) {
            Text('切换或恢复窗口').fontSize(15).fontColor('#FFFFFF')
              .width('100%').padding({ bottom: 4 })
            Button('返回主界面／设置')
              .id('host-task-launcher').width('100%').fontSize(13)
              .onClick((): void => {
                this.expanded = false;
                void HostWindowTasks.getInstance().showLauncher().catch((): void => {
                  promptAction.showToast({ message: '暂时无法返回主界面，请重试' });
                });
              })
            Scroll() {
              Column({ space: 6 }) {
                ForEach(this.tasks, (task: HostWindowTask): void => {
                  Button({ type: ButtonType.Normal }) {
                    Column({ space: 4 }) {
                      Text(this.label(task)).fontSize(14).fontColor('#FFFFFF')
                        .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
                      Text(task.minimized ? '已最小化 · 点击恢复' : '运行中 · 点击切换')
                        .fontSize(11).fontColor('#BBCADD').width('100%')
                    }.padding(12).alignItems(HorizontalAlign.Start)
                  }
                  .id(`host-task-${task.toplevelId}`)
                  .width('100%').borderRadius(10).backgroundColor('#34465F')
                  .enabled(!this.switching)
                  .onClick((): void => { void this.switchTo(task); })
                }, (task: HostWindowTask): string => task.key)
              }.width('100%')
            }.constraintSize({ maxHeight: 280 }).width('100%')
          }
          .padding(12).width(280).borderRadius(14).backgroundColor('#F01B2738')
        }
      }
      .alignItems(HorizontalAlign.End)
      // Never forward panel input to the game underneath.
      .onMouse((event: MouseEvent): void => { event.stopPropagation(); })
      .onTouch((event: TouchEvent): void => { event.stopPropagation(); })
      .onKeyEvent((event: KeyEvent): void => { event.stopPropagation(); })
    }
  }
}
