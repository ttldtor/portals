# Запуск корутины кнопкой {#ui_coroutines-ru}

[Английская версия](../ui-coroutines.md)

Необязательный экспериментальный адаптер [`UiAsync`](../../include/portals/async.hpp)
позволяет действию кнопки приостановиться, пока существующий [планировщик](scheduler.md)
продолжает обслуживать ввод. Программа меняет **Ready → Waiting → Completed**
после секундного таймера. **Exit** и F10 работают во время ожидания.
Повторный **Start** получает `WOULD_BLOCK`, пока прежний вызов ещё выполняется.

Фрагмент ниже предполагает `using namespace portals`, созданный `UiAsync &owner`,
`Application &app`, опубликованные `ui::Ref<Button> start` и `ui::Ref<Label> status`,
а также `delayMilliseconds = 1000`. Привязка выполняется внутри `ui::configure`;
её проверяемый результат возвращается в сцену. Создание владельца и очистка показаны
в полной программе ниже.

```cpp
UiTask finishLater(UiAsyncContext context, SchedulerTime deadline,
                   ui::Ref<Label> status) {
  status.text() = "Waiting";

  auto waited = co_await context.until(deadline);

  status.text() = waited ? "Completed" : "Wait failed";

  co_return waited;
}

// Inside the UI configuration callback, after successful owner creation.
return start.onClick([&owner, &app, status] {
  auto started = owner.start(
    finishLater(owner.context(), app.now() + delayMilliseconds, status));

  if (!started && started.error() != AsyncError::WOULD_BLOCK) {
    status.text() = "Start failed";
  }
});
```

Фрагмент показывает ожидание и смену надписи. Полная программа также сохраняет
типизированные ошибки, проверяет создание адаптера и освобождает его при остановке.
Захваченные по ссылке `owner` и `app` должны жить до отключения обработчика;
`Ref` наблюдает контрол и не продлевает его жизнь. Короткий fluent-адаптер
`ui::async` остаётся **TBD — M6**.

<details>
<summary>Полная программа</summary>

[Собираемый исходник](../examples/guide_async.cpp), подключение — `portals::core`.

```cpp
#include <portals/async.hpp>
#include <portals/ui.hpp>

#include <memory>
#include <optional>

namespace {
struct AsyncAction {
  std::unique_ptr<portals::UiAsync> owner;
  std::optional<portals::AsyncError> error;
};

portals::UiTask finishLater(portals::UiAsyncContext context,
                           portals::SchedulerTime deadline,
                           portals::ui::Ref<portals::Label> status,
                           std::shared_ptr<AsyncAction> action) {
  status.text() = "Waiting";

  auto waited = co_await context.until(deadline);

  if (!waited) {
    action->error = waited.error();
    status.text() = "Wait failed";

    co_return std::unexpected(waited.error());
  }

  status.text() = "Completed";

  co_return std::expected<void, portals::AsyncError>{};
}
} // namespace

int main() {
  using namespace portals;

  constexpr SchedulerTime delayMilliseconds = 1000;
  auto action = std::make_shared<AsyncAction>();
  ui::Ref<Label> status;
  ui::Ref<Button> start;
  ui::Ref<Button> exit;

  auto scene = ui::scene(ui::vbox(
    ui::label(status, "Ready"),
    ui::button(start, "Start"),
    ui::button(exit, "Exit")));

  scene.onStop([action] {
    if (action->owner) {
      auto closed = action->owner->close();

      if (!closed) {
        action->error = closed.error();
      }

      action->owner.reset();
    }
  });

  const int result = scene
    | ui::configure([action, status](ManagedScene &, Application &app) {
        auto created = UiAsync::create(app.scheduler(), app.tree(), app.root());

        if (!created) {
          action->error = created.error();
          status.text() = "Async setup failed";

          return;
        }

        action->owner = std::move(*created);
      })
    | ui::configure([action, status, start](ManagedScene &, Application &app) {
        auto bound = start.onClick([action, status, &app] {
          if (!action->owner) {
            return;
          }

          auto started = action->owner->start(
            finishLater(action->owner->context(), app.now() + delayMilliseconds, status, action));

          if (!started && started.error() != AsyncError::WOULD_BLOCK) {
            action->error = started.error();
            status.text() = "Start failed";
          }
        });

        return bound;
      })
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();

  return result != 0 || action->error ? 1 : 0;
}
```

</details>

Соберите полный [исходник](../examples/guide_async.cpp) с `portals::core` по
[руководству пакета](package.md). `Application::now()` использует те же монотонные
миллисекунды, что и таймер. Ожидание приостанавливает корутину без блокирующего sleep.
Это конкурентное выполнение корутин в UI-потоке; фоновые вычисления, I/O и новые
потоки здесь не запускаются. Долгое вычисление внутри корутины всё ещё блокирует ввод.

`scene.onStop` закрывает и уничтожает адаптер до освобождения приложения сцены.
Закрытие приостановленного адаптера уничтожает кадр корутины; тело не возобновляется
для вывода **Completed**. Свободная функция-корутина принимает владеющие параметры;
её `Ref` наблюдает надпись и не владеет виджетом. Изменения текста используют обычный
путь managed-диагностики и перерисовки. Ошибки запуска и таймера сохраняются в
`AsyncAction::error`; программа возвращает 1 после очистки. Корутина возвращает типизированный результат ожидания: ошибка устанавливает
исход адаптера `FAILED`, успешный возврат — `SUCCEEDED`.

## Ожидание результата настоящего источника

`co_await context.receive<T>(start)` ждёт неизменяемый результат источника.
UI вызывает `start` с пригодным для рабочего потока `AsyncReply<T>`; собственный
поток приложения публикует `shared_ptr<const T>` через `reply.complete(...)`
или типизированную ошибку через `reply.fail(...)`. Запуск, отмену и ожидание потока
организует приложение. При отказе приёма очередью сохраняйте точные reply и результат
для повторной попытки; принятие не означает возобновления корутины. Не вызывайте
вложенный dispatch из `start` и не изменяйте контролы из рабочего потока.
Точные контракты приёма и владения приведены ниже.

Fluent-адаптер `ui::async` для функций и корутинных лямбд — **TBD — M6** в
[плане возможностей](planned-features.md). Текущий API использует явного владельца,
как в примере. Сторонние awaiter и бесконечные генераторы не поддерживаются.

<details>
<summary>Справка корутин: владение, приём, ошибки и отмена</summary>

**Владение задачей**

`UiAsync::create(scheduler, tree, contextNode)` возвращает `expected<unique_ptr<UiAsync>, AsyncError>`. Область действия узла должна принадлежать переданному планировщику, иначе возвращается `INVALID_ARGUMENT`. Дерево и планировщик живут дольше адаптера. Удаление контекстного узла закрывает адаптер и отменяет ожидание.

Используйте свободную или статическую корутину с владеющими параметрами, как `finishLater` в полной программе выше. `UiTask` начинает тело после принятия адаптером. До этого кадром корутины владеет перемещаемая задача, после — адаптер UI. Создание, возобновление и уничтожение выполняются на потоке создания; уничтожение задачи или адаптера на другом потоке завершает процесс.

Параметры функции принадлежат кадру корутины. Замыкание корутинной лямбды автоматически не удерживается. Заимствованные ссылки на виджеты и представления нельзя переносить через приостановку. В полной программе кадр владеет копиями `context`, `deadline`, `status` и `action`. `shared_ptr<AsyncAction>` удерживает состояние действия; `status` — наблюдающий `Ref<Label>`, а `context` слабо наблюдает за адаптером. Они не продлевают жизнь дерева и планировщика. `scene.onStop` закрывает и уничтожает адаптер, пока эти сервисы ещё живы.

`owner.context()` возвращает объект доступа к ожиданиям адаптера. Он слабо наблюдает за адаптером и не продлевает его жизнь. Поддерживаются только его точные типы ожидания `AsyncTimer` и `AsyncReceive<T>`.

**Ожидание и доставка**

`co_await context.until(deadline)` ждёт абсолютного срока в миллисекундах часов планировщика и возвращает `expected<void, AsyncError>`. Уже наступивший срок также требует обслуживания планировщика и свободного лимита таймеров.

`co_await context.receive<T>(start)` регистрирует завершение до вызова обработчика `std::function<void(AsyncReply<T>)>` на UI. Результат — `expected<shared_ptr<const T>, AsyncError>`. Значение и достижимые данные должны оставаться неизменяемыми и допускать уничтожение любым освобождающим потоком. Ответ рабочего потока удерживает канал публикации и атомарное состояние принятия без кадра корутины и UI-обработчиков. Из обработчика запуска нельзя вложенно обслуживать планировщик.

`reply.complete(value)` принимает ненулевой неизменяемый снимок; `reply.fail(error)` публикует типизированную ошибку. Канал принимает один результат `QUEUED`. Вторая публикация до доставки возвращает `WOULD_BLOCK`, после доставки или отмены — `CLOSED`. Переполнение очереди и `NO_MEMORY` сохраняют возможность повторить: источник должен удержать точный ответ и результат. Принятый успех не заменяется другой ошибкой. Синхронное завершение тоже возобновляет тело только через планировщик.

`start(std::move(task), policy)` впервые возобновляет задачу на UI. `REJECT_RUNNING` сохраняет действующий вызов; `CANCEL_PREVIOUS` освобождает приостановленный предыдущий перед принятием нового. При исполняющемся теле оба возвращают `WOULD_BLOCK`. Отказ до принятия сохраняет задачу у вызывающего кода. Закрытие или удаление при первом возобновлении возвращает `CLOSED` после потребления задачи; отмена или замещение при принятии может вернуть `CANCELLED`.

`outcome()` отдельно сообщает поколение последней принятой задачи и `IDLE`, `RUNNING`, `SUCCEEDED`, `CANCELLED` или `FAILED`. Принятие задачи и очереди не доказывает успешного пользовательского эффекта.

Исключения тела и источника внутри адаптера переводятся в `NO_MEMORY` или `CALLBACK_FAILED`. Отказ регистрации возвращается из ожидания. Выделение кадра корутины и создание параметров или обработчика могут бросить до входа в адаптер. Остальные ошибки включают `INVALID_ARGUMENT`, `WRONG_THREAD`, `CLOSED`, `CANCELLED`, `WOULD_BLOCK` и `IDENTITY_EXHAUSTED`.

**Отмена и закрытие**

`cancel()` закрывает ожидающий канал. Приостановленный кадр корутины сразу уничтожается на UI; исполняющееся тело доходит до приостановки или возврата и затем освобождается. Отмена не возобновляет тело для очистки, не прерывает C++-код и не ожидает потоки источника. Используйте владеющие параметры и RAII; ресурсы источника закрываются по собственному контракту. Поколения отклоняют поздние ответы.

`close()` постоянно закрывает адаптер и идемпотентен. Последний результат доступен на UI после закрытия и удаления узла. Привязки действий отключаются до освобождения их захватов. Дерево и планировщик живут до уничтожения адаптера. Он занимает одну постоянную контекстную регистрацию и максимум одну регистрацию ожидания, подчиняясь обычным лимитам. Обновление кадра запрашивают пользовательские изменения.


</details>
