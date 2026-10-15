# Диагностика {#diagnostics-ru}

[Подробная английская версия](../diagnostics.md)

Portals записывает ограниченные метаданные прохождения действия: от ввода
до обработчика, изменения свойства и вывода кадра. Символы ввода, вставка,
буфер обмена, значения свойств, сообщения исключений, адреса и VT-байты
не записываются. Формат диагностики имеет собственную версию; семантический
replay использует PORTALS-TRACE 1.

## Подключение

```cpp
#include <portals/diagnostics.hpp>

// Keep ordinary error handling around every expected result in application code.
auto trace = std::make_shared<portals::Diagnostics>();
portals::SchedulerOptions options;
options.diagnostics = trace;
auto scheduler = portals::Scheduler::create(options).value();
auto tree = portals::WidgetTree::create(*scheduler).value();
portals::FrameWriter writer(output, {}, trace);
trace->enable({.capacity = 1024}).value();
```

По умолчанию диагностика выключена. Отключённые точки записи проверяют указатель
и при наличии регистратора его состояние; они не форматируют текст, не читают
часы, не выделяют кольцо и не исследуют данные. В очереди есть фиксированный
токен. `enable` резервирует кольцо; запись памяти не выделяет. Номера
последовательности задают порядок наблюдений. Часы и метрики Scheduler сохраняют
своё назначение.

Scheduler и его каналы публикации совместно владеют регистратором; дерево заимствует
Scheduler, writer удерживает регистратор. DiagnosticScope заимствует регистратор,
остаётся на создавшем потоке и завершается раньше него. Вложенный контекст
восстанавливается при обычном возврате и исключении.

## Путь действия

На границе декодированного ввода создавайте токен до публикации, включая
события, которым очередь впоследствии откажет:

```cpp
{
  portals::DiagnosticScope action(trace.get(),
    trace->beginInput(portals::DiagnosticInput::KEY));
  auto publication = inputPublisher.publish(owningInput);
  // Handle publication.error() normally. Diagnostics never retries delivery.
}
```

Scheduler удерживает токен в публикации и устанавливает его при вызове.
Router, фокус, действие, начало/возврат обработчика, Property и инвалидация
используют общий контекст. Прямой dispatch/invoke создаёт контекст при его
отсутствии. Другой поток может установить скопированный токен; токен не даёт
доступа к UI. Новый beginInput внутри контекста записывает родителя.

После диспетчеризации потребите запрос кадра, нарисуйте дерево и передайте
его токен writer. Несколько инвалидаций объединяются в один frame ID;
каждая причина записывается отдельно. Токен кадра указывает первую записанную
причину. Инвалидация рисования от компоновки относится к текущему кадру.
Записи writer отражают настоящий diff, submit и flush.
output_accepted подтверждает согласованное принятие транспортом;
физическое отображение на экране этим не подтверждается.

## Интерпретация

Enter на Button с присваиванием равного значения Property может дать:

```text
input input=key
queue_accepted queue=control
queue_dispatch queue=control
action action=1
target stableNodeId=<tree-session>:<slot>:<generation>
handler_started
property reason=value_unchanged
handler_returned reason=value_unchanged
frame_not_requested reason=frame_not_requested
completed reason=value_unchanged
queue_returned reason=outcome_unknown queue=control
```

value_unchanged относится к наблюдаемой операции Property. Без таких сведений
результат пользовательского обработчика остаётся outcome_unknown даже при
нормальном возврате. Поздний handler_failed сохраняет ранее записанное изменение.
frame_not_requested означает отсутствие нового запроса внутри данного scope;
старый ожидающий кадр мог уже существовать.

Причины: queue_full, replaced, cancelled, no_target, target_disabled,
stale_target, unbound_action, unhandled, handler_failed, value_unchanged,
outcome_unknown, frame_not_requested, diff_empty, output_failed.
Записывается реально выбранная маршрутизатором цель; восстановление фокуса
может выбрать другой узел. Session/slot/generation экспортируются без адресов
и действуют в пределах жизни дерева/регистратора. Замена публикации записывает
отмену старой и связь с новой. Причину отсутствия ввода до инструментированной
границы экспорт установить не может.

## Ёмкость и неполные цепочки

enable начинает новый интервал и очищает кольцо/потери; счётчики идентичности
и последовательности сохраняются без переполнения. disable сохраняет интервал.
Токен через границу интервала считается неполным. Исчерпание идентичности
навсегда останавливает приём с identity_exhausted.

Заполненное кольцо заменяет старейшую запись и увеличивает dropped_overflow.
Производитель пробует блокировку один раз: конкуренция учитывается как
dropped_contention. Копирование снимка держит блокировку; форматирование и I/O
выполняются отдельно. Память снимка ограничена ёмкостью. Анализ полноты в худшем
случае квадратичен по ёмкости; снимок берите между кадрами.

Экспорт содержит границы интервала, диапазон последовательностей, оба счётчика
потерь, насыщение, состояние и incomplete. Известная потеря помечает все цепочки
неполными. Также учитываются отсутствующие начало/завершение, ожидающие публикации
и вывод, токены другого интервала. Каждая принятая публикация требует собственной
queue_returned или queue_cancelled. Исключение обработчика даёт
queue_returned reason=handler_failed. Отсутствие записи не доказывает
отсутствие действия. Для выключенных точек полнота не учитывается.

## Экспорт

exportDiagnostics(snapshot) возвращает текст в памяти. Приложение выбирает
файл и обрабатывает ошибки. Снимок и сериализацию выполняйте между кадрами
или после восстановления терминала. Выводить экспорт в stdout/stderr во время
работы интерфейса в системном терминале нельзя.

```cpp
trace->disable();
auto saved = trace->snapshot();
// Restore/close the application's terminal before its final export.
auto text = portals::exportDiagnostics(saved);
std::ofstream file(chosenPath, std::ios::binary | std::ios::trunc);
file << text;
file.close();
if (!file) {
  // Report an export failure through the application's now-safe error channel.
}
```

Создание снимка и форматирование могут бросить bad_alloc, включая служебные данные отладочных итераторов. Возврат использует ограниченную копию вне блокировки.
Проверяйте открытие, запись и закрытие файла. validateDiagnosticVersion
проверяет заголовок версии до чтения полей; парсер воспроизводимых сценариев имеет отдельный API.
Проверочный пример portals_diagnostics_flow допускает путь экспорта, использует TestTerminal
и возвращает 2 при ошибке файла. Тестовая программа `portals_dashboard_check` поддерживает `--trace` после восстановления терминала;
смотрите [примеры](examples.md).
