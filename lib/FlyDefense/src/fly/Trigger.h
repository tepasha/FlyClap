/*
 * Тригер — сенсор "щось перетнуло зону" з миттєвою реакцією з переривання.
 *
 * Виконавець (наприклад, ClapLatch) реєструє обробник і "заряджає" тригер
 * (enableShot). Щойно ціль перетинає зону, тригер прямо з переривання викликає
 * обробник рівно один раз і сам розряджається. Латентність — мікросекунди,
 * без очікування головного циклу.
 *
 * Реалізації: IrCurtain (ІЧ-завіса). Свій тригер (п'єзо, ємнісний, лазерна
 * завіса на фотодіодах…) — нащадок цього класу, що викликає crossedFromIsr().
 */
#pragma once
#include "Module.h"

namespace fly {

class Trigger : public Module {
 public:
  typedef void (*Handler)(void *ctx);

  // Щось зараз у зоні (промінь перекрито).
  virtual bool blocked() const = 0;

  void setHandler(Handler h, void *ctx) {
    handler_ = h;
    ctx_ = ctx;
  }
  // Дозволити один постріл з переривання.
  void enableShot() { shotEnabled_ = true; }
  void cancelShot() { shotEnabled_ = false; }
  bool shotEnabled() const { return shotEnabled_; }

 protected:
  // Викликати з переривання, коли зону перетнули (після антидребезгу).
  void crossedFromIsr() {
    if (!shotEnabled_ || !handler_) return;
    shotEnabled_ = false;
    handler_(ctx_);
  }

  volatile bool shotEnabled_ = false;

 private:
  Handler handler_ = nullptr;
  void *ctx_ = nullptr;
};

}  // namespace fly
