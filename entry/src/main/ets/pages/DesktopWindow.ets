import { DesktopLayer } from '../components/DesktopLayer';

@Entry
@Component
struct DesktopPage {
  build() {
    Stack() {
      DesktopLayer({ floating: false })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
