#include "components.h"

#include <XBase/Json.h>
#include <XBase/Log.h>

#include <string>
#include <utility>

namespace PanelSample::Components {
namespace {

double g_enabled = 0.0;
std::string g_name = "Tommy";
std::string g_range = "[20,80]";
std::string g_date = "2026-01-01";
std::string g_choice = "Apple";
std::string g_submission = "{}";

XBase::Panel::ComponentNode Node(const char* component, const char* text = "") {
    XBase::Panel::ComponentNode node;
    node.component = component;
    node.text = text;
    return node;
}

XBase::Panel::Control Control(const char* id, XBase::Panel::ComponentNode node) {
    XBase::Panel::Control control;
    control.kind = XBase::Panel::ControlKind::Component;
    control.id = std::string("panelsample.components.") + id;
    control.component = std::move(node);
    return control;
}

}

XBase::Panel::Page BuildPage() {
    using XBase::Panel::ComponentBindingKind;
    XBase::Panel::Page page;
    page.id = "components";
    page.label = "组件";
    XBase::Panel::Section inputs;
    inputs.id = "componentInputs";
    inputs.label = "输入";

    XBase::Panel::ComponentNode toggle = Node("Switch");
    toggle.props.Set("aria-label", "启用");
    toggle.bindings.push_back({"panelsample.components.enabled", "checked", "onCheckedChange", ComponentBindingKind::Value});
    inputs.controls.push_back(Control("toggle", std::move(toggle)));

    XBase::Panel::ComponentNode input = Node("Input");
    input.props.Set("aria-label", "名字");
    input.bindings.push_back({"panelsample.components.name", "value", "onChange", ComponentBindingKind::Text});
    inputs.controls.push_back(Control("input", std::move(input)));

    XBase::Panel::ComponentNode slider = Node("Slider");
    slider.props.Set("aria-label", "范围").Set("min", 0).Set("max", 100).Set("step", 1);
    slider.props.Set("value", XBase::Json::Value::Parse("[20,80]"));
    slider.bindings.push_back({"panelsample.components.range", "value", "onValueChange", ComponentBindingKind::Json});
    inputs.controls.push_back(Control("range", std::move(slider)));

    XBase::Panel::ComponentNode date = Node("DatePicker");
    date.bindings.push_back({"panelsample.components.date", "value", "onValueChange", ComponentBindingKind::Text});
    inputs.controls.push_back(Control("date", std::move(date)));

    XBase::Panel::ComponentNode combobox = Node("Combobox");
    combobox.props.Set("items", XBase::Json::Value::Parse("[\"Apple\",\"Banana\",\"Cherry\"]"));
    combobox.bindings.push_back({"panelsample.components.choice", "value", "onValueChange", ComponentBindingKind::Text});
    XBase::Panel::ComponentNode comboInput = Node("ComboboxInput");
    comboInput.props.Set("aria-label", "选项");
    combobox.children.push_back(std::move(comboInput));
    XBase::Panel::ComponentNode comboContent = Node("ComboboxContent");
    XBase::Panel::ComponentNode list = Node("ComboboxList");
    XBase::Panel::ComponentNode item = Node("ComboboxItem");
    item.props.Set("value", XBase::Json::Value::Parse("{\"$arg\":\"\"}"));
    item.textPath = "$value";
    list.templates.emplace("children", std::move(item));
    comboContent.children.push_back(std::move(list));
    comboContent.children.push_back(Node("ComboboxEmpty", "没有结果"));
    combobox.children.push_back(std::move(comboContent));
    inputs.controls.push_back(Control("choiceRoot", std::move(combobox)));
    page.sections.push_back(std::move(inputs));

    XBase::Panel::Section composition;
    composition.id = "componentComposition";
    composition.label = "操作";
    XBase::Panel::ComponentNode dialog = Node("Dialog");
    XBase::Panel::ComponentNode trigger = Node("DialogTrigger");
    trigger.props.Set("asChild", true);
    trigger.children.push_back(Node("Button", "打开对话框"));
    dialog.children.push_back(std::move(trigger));
    XBase::Panel::ComponentNode content = Node("DialogContent");
    XBase::Panel::ComponentNode header = Node("DialogHeader");
    header.children.push_back(Node("DialogTitle", "操作确认"));
    header.children.push_back(Node("DialogDescription", "确认执行这次操作"));
    content.children.push_back(std::move(header));
    XBase::Panel::ComponentNode action = Node("Button", "执行");
    action.bindings.push_back({"panelsample.components.action", "", "onClick", ComponentBindingKind::Action});
    content.children.push_back(std::move(action));
    dialog.children.push_back(std::move(content));
    composition.controls.push_back(Control("dialog", std::move(dialog)));

    XBase::Panel::ComponentNode form = Node("Form");
    form.props.Set("defaultValues", XBase::Json::Value::Parse("{\"name\":\"Tommy\"}"));
    form.bindings.push_back({"panelsample.components.submission", "", "onSubmit", ComponentBindingKind::Json});
    XBase::Panel::ComponentNode field = Node("FormField");
    field.props.Set("name", "name").Set("rules", XBase::Json::Value::Parse("{\"required\":\"请输入名字\"}"));
    XBase::Panel::ComponentNode formItem = Node("FormItem");
    formItem.children.push_back(Node("FormLabel", "名字"));
    XBase::Panel::ComponentNode formControl = Node("FormControl");
    formControl.children.push_back(Node("Input"));
    formItem.children.push_back(std::move(formControl));
    formItem.children.push_back(Node("FormMessage"));
    field.children.push_back(std::move(formItem));
    form.children.push_back(std::move(field));
    XBase::Panel::ComponentNode submit = Node("Button", "提交");
    submit.props.Set("type", "submit");
    form.children.push_back(std::move(submit));
    composition.controls.push_back(Control("form", std::move(form)));
    page.sections.push_back(std::move(composition));

    XBase::Panel::Section data;
    data.id = "componentData";
    data.label = "数据";
    XBase::Panel::ComponentNode table = Node("DataTable");
    table.props.Set("columns", XBase::Json::Value::Parse("[{\"key\":\"name\",\"label\":\"名字\"},{\"key\":\"value\",\"label\":\"数量\"}]"));
    table.props.Set("data", XBase::Json::Value::Parse("[{\"name\":\"Tommy\",\"value\":20},{\"name\":\"Claude\",\"value\":80}]"));
    data.controls.push_back(Control("table", std::move(table)));
    XBase::Panel::ComponentNode chart = Node("ChartContainer");
    chart.props.Set("config", XBase::Json::Value::Parse("{\"value\":{\"label\":\"数量\",\"color\":\"#16a34a\"}}"));
    chart.props.Set("style", XBase::Json::Value::Parse("{\"height\":220,\"width\":\"100%\"}"));
    XBase::Panel::ComponentNode barChart = Node("BarChart");
    barChart.props.Set("data", XBase::Json::Value::Parse("[{\"name\":\"A\",\"value\":20},{\"name\":\"B\",\"value\":80}]"));
    XBase::Panel::ComponentNode axis = Node("XAxis");
    axis.props.Set("dataKey", "name");
    barChart.children.push_back(std::move(axis));
    XBase::Panel::ComponentNode bar = Node("Bar");
    bar.props.Set("dataKey", "value").Set("fill", "var(--color-value)");
    barChart.children.push_back(std::move(bar));
    XBase::Panel::ComponentNode tooltip = Node("ChartTooltip");
    tooltip.slots["content"].push_back(Node("ChartTooltipContent"));
    barChart.children.push_back(std::move(tooltip));
    chart.children.push_back(std::move(barChart));
    data.controls.push_back(Control("chart", std::move(chart)));
    page.sections.push_back(std::move(data));
    return page;
}

bool Bind() {
    bool ok = XBase::Panel::BindValue(
        "panelsample.components.enabled", [] { return g_enabled; }, [](double value) { g_enabled = value; });
    for (const auto& [id, state] : {
             std::pair<const char*, std::string*>{"name", &g_name},
             {"range", &g_range}, {"date", &g_date}, {"choice", &g_choice}}) {
        ok = XBase::Panel::BindText(
            std::string("panelsample.components.") + id,
            [state] { return *state; },
            [state](const std::string& value) { *state = value; }) && ok;
    }
    ok = XBase::Panel::BindText(
        "panelsample.components.submission",
        [] { return g_submission; },
        [](const std::string& value) {
            g_submission = value;
            XBase::Log::Info(std::string("Form: ") + g_submission);
            XBase::Panel::NotifyTextChanged("panelsample.notes", g_submission);
        }) && ok;
    ok = XBase::Panel::BindAction(
        "panelsample.components.action",
        [] { XBase::Log::Info("组件动作已执行"); }) && ok;
    return ok;
}

}
