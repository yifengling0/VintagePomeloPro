import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';
import { DeckLaunchArtwork, copyDeckCoverBounds } from '../model/DeckLaunchArtwork';

export interface DeckLaunchFeedback extends DeckLaunchArtwork {
  title: string;
  message: string;
  hint?: string;
  /** Classic launcher renders an in-theme preparing panel instead of the deck surface. */
  classic?: boolean;
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
    return { title: this.value.title, cover: this.value.cover, message: this.value.message,
      hint: this.value.hint, builtin: this.value.builtin, classic: this.value.classic,
      origin: copyDeckCoverBounds(this.value.origin) };
  }

  subscribe(listener: DeckLaunchFeedbackListener): void { this.listeners.add(listener); listener(this.snapshot()); }
  unsubscribe(listener: DeckLaunchFeedbackListener): void { this.listeners.delete(listener); }
  private publish(): void { this.listeners.forEach((listener: DeckLaunchFeedbackListener): void => listener(this.snapshot())); }

  begin(feedback: DeckLaunchFeedback): number {
    const ticket = ++this.ticket;
    this.value = { title: feedback.title, cover: feedback.cover, message: feedback.message,
      hint: feedback.hint, builtin: feedback.builtin, classic: feedback.classic,
      origin: copyDeckCoverBounds(feedback.origin) };
    this.publish();
    void InputOverlayWindowCoordinator.getInstance().setLaunchBusy(true).catch((): void => {});
    return ticket;
  }

  update(ticket: number, message: string, hint?: string): void {
    if (ticket !== this.ticket || !this.value) return;
    const nextHint = hint ?? this.value.hint;
    if (message === this.value.message && nextHint === this.value.hint) return;
    this.value.message = message;
    this.value.hint = nextHint;
    this.publish();
  }

  finish(ticket: number): void {
    if (ticket !== this.ticket || !this.value) return;
    this.value = null; this.publish();
    void InputOverlayWindowCoordinator.getInstance().setLaunchBusy(false).catch((): void => {});
  }
}
