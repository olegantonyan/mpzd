#include "app.h"

#include <stddef.h>

void (*app_on_change)(void) = NULL;

void app_changed(void) {
  if (app_on_change) {
    app_on_change();
  }
}
