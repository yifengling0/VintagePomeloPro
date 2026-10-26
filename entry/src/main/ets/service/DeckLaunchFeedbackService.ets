import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';

export interface DeckLaunchFeedback {
  title: string;
  cover: string;
  message: string;
  builtin?: Resource;
}

export type DeckLaunchFeedbackListener = (feedback: DeckLaunchFeedback | null) => void;

// Shared by the launcher and the retained input window above native frames.
// A completion from an earlier request must never dismiss a newer launch.
export class DeckLaunchFeedbackService {
  private static instance: DeckLaunchFeedbackService | null = null;
  private ticket: number = 0;
  private value: DeckLaunchFeedback | null = null;
  private listeners: Set<DeckLaunchFeedbackListener> = new Set<DeckLaunchFeedbackListener>();

  static getInstance(): DeckLaunchFeedbackService {
    if (!DeckLaunchFeedbackService.instance) DeckLaunchFeedbackService.instance = new DeckLaunchFeedbackService();
    return DeckLaunchFeedbackService.instance;
  }

  snapshot(): DeckLaunchFeedback | null {
    if (!this.value) return null;
    return { title: this.value.title, cover: this.value.cover, message: this.value.message, builtin: this.value.builtin };
  }

  subscribe(listener: DeckLaunchFeedbackListener): void { this.listeners.add(listener); listener(this.snapshot()); }
  unsubscribe(listener: DeckLaunchFeedbackListener): void { this.listeners.delete(listener); }
  private publish(): void { this.listeners.forEach((listener: DeckLaunchFeedbackListener): void => listener(this.snapshot())); }

  begin(feedback: DeckLaunchFeedback): number {
    const ticket = ++this.ticket;
    this.value = { title: feedback.title, cover: feedback.cover, message: feedback.message, builtin: feedback.builtin };
    this.publish();
    void InputOverlayWindowCoordinator.getInstance().setLaunchBusy(true).catch((): void => {});
    return ticket;
  }

  update(ticket: number, message: string): void {
    if (ticket !== this.ticket || !this.value || message === this.value.message) return;
    this.value.message = message; this.publish();
  }

  finish(ticket: number): void {
    if (ticket !== this.ticket || !this.value) return;
    this.value = null; this.publish();
    void InputOverlayWindowCoordinator.getInstance().setLaunchBusy(false).catch((): void => {});
  }
}
