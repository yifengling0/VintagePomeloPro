#pragma once
// The real registry remains active; suppress /proc polling in this test so
// each scenario chooses which real waitpid consumer observes the child exit.
inline bool PhoneAdapter_IsPhoneMode() { return false; }
