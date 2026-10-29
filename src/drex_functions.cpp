#include "drex_functions.hpp"

#include "drex_client.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

static constexpr const char *DEFAULT_MODEL = "drex-v2.0";
//! Drex's limits: questions per request, and labels per choice or levels per score
static constexpr idx_t MAX_QUESTIONS = 512;
static constexpr idx_t MIN_LABELS = 2;
static constexpr idx_t MAX_LABELS = 255;

//! The versions that read images. Drex's GET /v1/models has no image field, so the extension keeps the list; a listed
//! model reads images when it is one of these or its alias_for names one. OpenRouter lists each model's modalities
static bool IsImageModel(const string &served_by) {
	return served_by == "drex-max" || served_by == "drex-max-v1.1";
}

//===--------------------------------------------------------------------===//
// Errors
//===--------------------------------------------------------------------===//

//! Every error the functions raise starts with the function's name
template <typename... ARGS>
static InvalidInputException DrexError(const string &function, const string &message, ARGS... params) {
	return InvalidInputException(function + ": " + message, params...);
}

//! Runs `call`, prefixing any error it throws with the function's name, keeping the error's type
template <class CALL>
static auto WithFunctionName(const string &function, CALL &&call) -> decltype(call()) {
	try {
		return call();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (StringUtil::StartsWith(error.RawMessage(), function + ":")) {
			throw;
		}
		error.Throw(function + ": ");
	}
}

//===--------------------------------------------------------------------===//
// Questions
//===--------------------------------------------------------------------===//

enum class DrexQuestionType : uint8_t { NOUL, CHOICE, SCORE };

static string QuestionTypeName(DrexQuestionType type) {
	switch (type) {
	case DrexQuestionType::NOUL:
		return "noul";
	case DrexQuestionType::CHOICE:
		return "choice";
	default:
		return "score";
	}
}

//! One label of a noul, choice or score, in prompt_jev's forms: a plain label, or a label with a description
struct DrexLabel {
	string label;
	bool has_description = false;
	string description;
};

struct DrexQuestion {
	string id;
	DrexQuestionType type;
	string instructions;
	vector<DrexLabel> labels;
	//! Whether the labels were written as Drex's object of label to description, where a noul may describe only one
	bool labels_from_object = false;
};

//! The accepted criteria forms, for the error that names them
static string CriteriaShapeError(const string &function, const string &name) {
	return function + ": " + name + " must be a VARCHAR[] of labels or a STRUCT(label VARCHAR, description VARCHAR)[]";
}

static DrexLabel MakeLabel(const string &function, const string &name, const Value &label, const Value &description) {
	if (label.IsNull()) {
		throw DrexError(function, "%s labels must not be NULL", name);
	}
	DrexLabel result;
	result.label = StringValue::Get(label);
	if (!description.IsNull()) {
		result.has_description = true;
		result.description = StringValue::Get(description);
	}
	return result;
}

//! The labels of a criteria argument in prompt_jev's forms, a VARCHAR[] or a STRUCT(label, description)[]
static vector<DrexLabel> LabelsFromValue(const string &function, const string &name, const Value &value) {
	auto &type = value.type();
	if (type.id() != LogicalTypeId::LIST || value.IsNull()) {
		throw InvalidInputException(CriteriaShapeError(function, name));
	}
	auto &child_type = ListType::GetChildType(type);
	optional_idx label_index;
	optional_idx description_index;
	if (child_type.id() == LogicalTypeId::STRUCT) {
		for (idx_t i = 0; i < StructType::GetChildCount(child_type); i++) {
			auto &field = StructType::GetChildName(child_type, i);
			auto field_type = StructType::GetChildType(child_type, i).id();
			bool is_text = field_type == LogicalTypeId::VARCHAR || field_type == LogicalTypeId::SQLNULL;
			if (field == "label" && is_text) {
				label_index = i;
			} else if (field == "description" && is_text) {
				description_index = i;
			} else {
				throw InvalidInputException(CriteriaShapeError(function, name));
			}
		}
		if (!label_index.IsValid()) {
			throw InvalidInputException(CriteriaShapeError(function, name));
		}
	} else if (child_type.id() != LogicalTypeId::VARCHAR && child_type.id() != LogicalTypeId::SQLNULL) {
		throw InvalidInputException(CriteriaShapeError(function, name));
	}

	vector<DrexLabel> labels;
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull() || !label_index.IsValid()) {
			labels.push_back(MakeLabel(function, name, child, Value()));
			continue;
		}
		auto &fields = StructValue::GetChildren(child);
		auto description = description_index.IsValid() ? fields[description_index.GetIndex()] : Value();
		labels.push_back(MakeLabel(function, name, fields[label_index.GetIndex()], description));
	}
	return labels;
}

//! The labels of criteria written in JSON: prompt_jev's forms, an array of labels or of {label, description} objects,
//! or, for a noul or a choice, Drex's own object of label to description
static vector<DrexLabel> LabelsFromJSON(const string &function, const string &name, const JSONValue &value,
                                        bool allow_object) {
	auto shape_error = [&]() {
		return InvalidInputException(CriteriaShapeError(function, name) +
		                             (allow_object ? ", or an object of label to description" : ""));
	};
	auto text_or_null = [&](const JSONValue &field) {
		if (!field.IsValid() || field.IsNull()) {
			return Value();
		}
		if (!field.IsString()) {
			throw shape_error();
		}
		return Value(field.GetString());
	};

	vector<DrexLabel> labels;
	if (value.IsArray()) {
		value.IterateArray([&](JSONValue element) {
			if (element.IsObject()) {
				element.IterateObject([&](const string &key, JSONValue) {
					if (key != "label" && key != "description") {
						throw shape_error();
					}
				});
				labels.push_back(MakeLabel(function, name, text_or_null(element.GetMember("label")),
				                           text_or_null(element.GetMember("description"))));
			} else {
				labels.push_back(MakeLabel(function, name, text_or_null(element), Value()));
			}
		});
	} else if (value.IsObject() && allow_object) {
		value.IterateObject([&](const string &key, JSONValue description) {
			labels.push_back(MakeLabel(function, name, Value(key), text_or_null(description)));
		});
	} else {
		throw shape_error();
	}
	return labels;
}

//! Checks a question's labels against Drex's and prompt_jev's rules
static void CheckLabels(const string &function, const string &name, const DrexQuestion &question) {
	auto &labels = question.labels;
	if (question.type == DrexQuestionType::NOUL) {
		// No labels is a noul without criteria
		bool has_true = false;
		bool has_false = false;
		bool valid = true;
		for (auto &label : labels) {
			bool &seen = label.label == "true" ? has_true : has_false;
			valid = valid && (label.label == "true" || label.label == "false") && !seen;
			seen = true;
		}
		bool complete = labels.empty() || question.labels_from_object || (has_true && has_false);
		if (!valid || !complete) {
			vector<string> found;
			for (auto &label : labels) {
				found.push_back(label.label);
			}
			throw DrexError(function, "%s labels must be exactly true and false, got [%s]", name,
			                StringUtil::Join(found, ", "));
		}
		return;
	}
	auto kind = question.type == DrexQuestionType::CHOICE ? "labels" : "levels";
	if (labels.size() < MIN_LABELS || labels.size() > MAX_LABELS) {
		throw DrexError(function, "%s takes %llu to %llu %s, got %llu", name, MIN_LABELS, MAX_LABELS, kind,
		                labels.size());
	}
	unordered_set<string> seen;
	for (auto &label : labels) {
		if (label.label.empty()) {
			throw DrexError(function, "%s %s must not be empty", name, kind);
		}
		if (!seen.insert(label.label).second) {
			throw DrexError(function, "%s %s must be unique; '%s' is a duplicate", name, kind, label.label);
		}
		if (question.type == DrexQuestionType::SCORE && label.has_description) {
			throw DrexError(function, "score levels take no description; describe them in instructions");
		}
	}
}

//! Checks what Drex would refuse in a noul: one with neither instructions nor a description of true or false
static void CheckNoulHasText(const string &function, const string &name, const DrexQuestion &question) {
	if (question.type != DrexQuestionType::NOUL || !question.instructions.empty()) {
		return;
	}
	for (auto &label : question.labels) {
		if (label.has_description && !label.description.empty()) {
			return;
		}
	}
	throw DrexError(function, "%s needs instructions, or a description of true or false", name);
}

static DrexQuestionType ParseQuestionType(const string &function, const string &name, const string &type) {
	if (type == "noul") {
		return DrexQuestionType::NOUL;
	}
	if (type == "choice") {
		return DrexQuestionType::CHOICE;
	}
	if (type == "score") {
		return DrexQuestionType::SCORE;
	}
	if (type == "multiple_choice" || type == "box") {
		throw DrexError(function, "%s: %s questions are not supported, since Drex has no %s question", name, type,
		                type);
	}
	throw DrexError(function, "%s.type must be noul, choice or score, not '%s'", name, type);
}

//! The questions of `questions :=` written as a STRUCT of STRUCT(type, instructions, criteria)
static vector<DrexQuestion> QuestionsFromStruct(const string &function, const Value &value) {
	vector<DrexQuestion> questions;
	auto &type = value.type();
	auto &entries = StructValue::GetChildren(value);
	for (idx_t i = 0; i < entries.size(); i++) {
		DrexQuestion question;
		question.id = StructType::GetChildName(type, i).GetIdentifierName();
		auto name = "questions." + question.id;
		auto &entry = entries[i];
		if (entry.type().id() != LogicalTypeId::STRUCT || entry.IsNull()) {
			throw DrexError(function, "%s must be a STRUCT(type, instructions, criteria)", name);
		}
		Value question_type;
		Value criteria;
		auto &fields = StructValue::GetChildren(entry);
		for (idx_t f = 0; f < fields.size(); f++) {
			auto &field = StructType::GetChildName(entry.type(), f);
			if (field == "type") {
				question_type = fields[f];
			} else if (field == "instructions") {
				if (!fields[f].IsNull() && fields[f].type().id() != LogicalTypeId::VARCHAR) {
					throw DrexError(function, "%s.instructions must be a VARCHAR", name);
				}
				question.instructions = fields[f].IsNull() ? "" : StringValue::Get(fields[f]);
			} else if (field == "criteria") {
				criteria = fields[f];
			} else {
				throw DrexError(function, "%s has an unknown field '%s'; use type, instructions and criteria", name,
				                field.GetIdentifierName());
			}
		}
		if (question_type.IsNull() || question_type.type().id() != LogicalTypeId::VARCHAR) {
			throw DrexError(function, "%s.type must be noul, choice or score", name);
		}
		question.type = ParseQuestionType(function, name, StringValue::Get(question_type));
		if (!criteria.IsNull()) {
			question.labels = LabelsFromValue(function, name + ".criteria", criteria);
		} else if (question.type != DrexQuestionType::NOUL) {
			throw DrexError(function, "%s needs criteria", name);
		}
		questions.push_back(std::move(question));
	}
	return questions;
}

//! The questions of `questions :=` written as a JSON object of {type, instructions, criteria} objects
static vector<DrexQuestion> QuestionsFromJSON(const string &function, const string &json) {
	JSONParseError parse_error;
	auto document = JSONDocument::TryParse(json.c_str(), json.size(), parse_error);
	if (!document) {
		throw DrexError(function, "questions is not valid JSON: %s", parse_error.message);
	}
	auto root = document->GetRoot();
	if (!root.IsObject()) {
		throw DrexError(function, "questions must be a JSON object of question id to question");
	}
	vector<DrexQuestion> questions;
	root.IterateObject([&](const string &id, JSONValue entry) {
		DrexQuestion question;
		question.id = id;
		auto name = "questions." + id;
		if (!entry.IsObject()) {
			throw DrexError(function, "%s must be an object with type, instructions and criteria", name);
		}
		entry.IterateObject([&](const string &key, JSONValue) {
			if (key != "type" && key != "instructions" && key != "criteria") {
				throw DrexError(function, "%s has an unknown key '%s'; use type, instructions and criteria", name, key);
			}
		});
		auto question_type = entry.GetMember("type");
		if (!question_type.IsString()) {
			throw DrexError(function, "%s.type must be noul, choice or score", name);
		}
		question.type = ParseQuestionType(function, name, question_type.GetString());
		auto instructions = entry.GetMember("instructions");
		if (instructions.IsValid() && !instructions.IsNull()) {
			if (!instructions.IsString()) {
				throw DrexError(function, "%s.instructions must be a string", name);
			}
			question.instructions = instructions.GetString();
		}
		auto criteria = entry.GetMember("criteria");
		if (criteria.IsValid() && !criteria.IsNull()) {
			question.labels =
			    LabelsFromJSON(function, name + ".criteria", criteria, question.type != DrexQuestionType::SCORE);
			question.labels_from_object = criteria.IsObject();
		} else if (question.type != DrexQuestionType::NOUL) {
			throw DrexError(function, "%s needs criteria", name);
		}
		questions.push_back(std::move(question));
	});
	return questions;
}

//! The `questions` object Drex takes, with `instructions` always a string
static string QuestionsToJSON(const vector<DrexQuestion> &questions) {
	JSONWriter writer;
	auto root = writer.CreateObject();
	for (auto &question : questions) {
		auto object = writer.CreateObject();
		object.AddString("type", QuestionTypeName(question.type));
		object.AddString("instructions", question.instructions);
		auto description = [&](const DrexLabel &label) {
			return label.has_description ? writer.CreateString(label.description) : writer.CreateNull();
		};
		if (question.type == DrexQuestionType::SCORE) {
			auto levels = writer.CreateArray();
			for (auto &label : question.labels) {
				levels.AppendString(label.label);
			}
			object.Add("criteria", levels);
		} else if (question.type == DrexQuestionType::CHOICE) {
			auto criteria = writer.CreateObject();
			for (auto &label : question.labels) {
				criteria.Add(label.label, description(label));
			}
			object.Add("criteria", criteria);
		} else {
			// Labels alone are the same as no criteria
			bool described = false;
			auto criteria = writer.CreateObject();
			for (auto &label : question.labels) {
				described = described || label.has_description;
				criteria.Add(label.label, description(label));
			}
			if (described) {
				object.Add("criteria", criteria);
			}
		}
		root.Add(question.id, object);
	}
	writer.SetRoot(root);
	return writer.ToString();
}

//===--------------------------------------------------------------------===//
// Answers, as prompt_jev returns them
//===--------------------------------------------------------------------===//

static LogicalType ChoiceProbabilityType() {
	return LogicalType::STRUCT({{"value", LogicalType::VARCHAR}, {"probability", LogicalType::DOUBLE}});
}

static LogicalType ScoreProbabilityType() {
	return LogicalType::STRUCT(
	    {{"index", LogicalType::UINTEGER}, {"value", LogicalType::VARCHAR}, {"probability", LogicalType::DOUBLE}});
}

static LogicalType AnswerType(DrexQuestionType type) {
	switch (type) {
	case DrexQuestionType::NOUL:
		return LogicalType::DOUBLE;
	case DrexQuestionType::CHOICE:
		return LogicalType::STRUCT({{"choice", LogicalType::VARCHAR},
		                            {"probabilities", LogicalType::LIST(ChoiceProbabilityType())},
		                            {"confidence", LogicalType::DOUBLE}});
	default:
		return LogicalType::STRUCT({{"score", LogicalType::DOUBLE},
		                            {"probabilities", LogicalType::LIST(ScoreProbabilityType())},
		                            {"confidence", LogicalType::DOUBLE}});
	}
}

//! The member of a JSON object under a key from the query, a label or a question id. GetMember cuts its key at the
//! first NUL byte, so a key holding one is matched in full
static JSONValue MemberOf(const JSONValue &object, const string &key) {
	if (key.find('\0') == string::npos) {
		return object.GetMember(key);
	}
	JSONValue member;
	if (object.IsObject()) {
		object.IterateObject([&](const string &name, JSONValue value) {
			if (!member.IsValid() && name == key) {
				member = value;
			}
		});
	}
	return member;
}

static double AnswerNumber(const string &function, const DrexQuestion &question, const JSONValue &answer,
                           const string &key) {
	auto value = MemberOf(answer, key);
	if (!value.IsNumber()) {
		throw IOException("%s: Drex's answer to question '%s' has no number %s", function, question.id, key);
	}
	return value.GetNumber();
}

//! Drex's answer to one question as prompt_jev's value: a choice's probabilities sorted by label, a score's by index
static Value AnswerValue(const string &function, const DrexQuestion &question, const JSONValue &answer) {
	if (!answer.IsObject()) {
		throw IOException("%s: Drex's response has no answer to question '%s'", function, question.id);
	}
	if (question.type == DrexQuestionType::NOUL) {
		return Value::DOUBLE(AnswerNumber(function, question, answer, "noul"));
	}
	auto probabilities = answer.GetMember("probabilities");
	auto confidence = Value::DOUBLE(AnswerNumber(function, question, answer, "confidence"));
	vector<Value> list;
	if (question.type == DrexQuestionType::CHOICE) {
		auto choice = answer.GetMember("choice");
		if (!choice.IsString()) {
			throw IOException("%s: Drex's answer to question '%s' has no choice", function, question.id);
		}
		vector<string> labels;
		for (auto &label : question.labels) {
			labels.push_back(label.label);
		}
		std::sort(labels.begin(), labels.end());
		for (auto &label : labels) {
			list.push_back(
			    Value::STRUCT(ChoiceProbabilityType(),
			                  {Value(label), Value::DOUBLE(AnswerNumber(function, question, probabilities, label))}));
		}
		return Value::STRUCT(
		    AnswerType(question.type),
		    {Value(choice.GetString()), Value::LIST(ChoiceProbabilityType(), std::move(list)), confidence});
	}
	for (idx_t i = 0; i < question.labels.size(); i++) {
		list.push_back(Value::STRUCT(ScoreProbabilityType(),
		                             {Value::UINTEGER(NumericCast<uint32_t>(i)), Value(question.labels[i].label),
		                              Value::DOUBLE(AnswerNumber(function, question, probabilities, to_string(i)))}));
	}
	return Value::STRUCT(AnswerType(question.type), {Value::DOUBLE(AnswerNumber(function, question, answer, "score")),
	                                                 Value::LIST(ScoreProbabilityType(), std::move(list)), confidence});
}

//===--------------------------------------------------------------------===//
// Models
//===--------------------------------------------------------------------===//

//! A model GET /v1/models lists
struct DrexListedModel {
	//! The name a request sends
	string name;
	bool reads_images = false;
};

//! GET /v1/models for a base URL, cached per database instance
class DrexModelList : public ObjectCacheEntry {
public:
	//! In the order the base URL lists them
	vector<DrexListedModel> models;
	//! Whether the list is OpenRouter's, whose names are under nace-ai/
	bool open_router = false;

	static string ObjectType() {
		return "drex_models";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// A few hundred bytes, never worth evicting
		return optional_idx();
	}

	//! The listed model a request for `model` is sent as: on OpenRouter, a name without an owner is under nace-ai/, as
	//! the client sends it
	optional_ptr<const DrexListedModel> Find(const string &model) const {
		auto name = open_router && model.find('/') == string::npos ? "nace-ai/" + model : model;
		for (auto &entry : models) {
			if (entry.name == name) {
				return &entry;
			}
		}
		return nullptr;
	}
};

static shared_ptr<DrexModelList> FetchModels(const string &function, const DrexClient &client, ClientContext &context) {
	auto result = WithFunctionName(function, [&]() { return client.Models(function, &context); });
	if (!result.Success()) {
		throw IOException("%s: could not list Drex's models: %s", function, result.error);
	}
	auto malformed = [&]() {
		return IOException("%s: GET /v1/models answered with an unexpected body", function);
	};
	JSONParseError parse_error;
	auto document = JSONDocument::TryParse(result.json.c_str(), result.json.size(), parse_error);
	if (!document) {
		throw malformed();
	}
	auto root = document->GetRoot();
	auto list = make_shared_ptr<DrexModelList>();
	auto drex_models = root.GetMember("models");
	auto open_router_models = root.GetMember("data");
	if (drex_models.IsArray()) {
		// Drex's: {"models": [{"name", "alias_for"}]}. A model reads images when it, or the version its alias_for
		// names, is an image model
		drex_models.IterateArray([&](JSONValue model) {
			auto name = model.GetMember("name");
			auto alias_for = model.GetMember("alias_for");
			if (!name.IsString()) {
				throw malformed();
			}
			auto served_by = alias_for.IsString() ? alias_for.GetString() : name.GetString();
			list->models.push_back({name.GetString(), IsImageModel(served_by)});
		});
	} else if (open_router_models.IsArray()) {
		// OpenRouter's: {"data": [{"id", "architecture": {"input_modalities"}}]}, of which only Nace's models are
		// Drex
		list->open_router = true;
		open_router_models.IterateArray([&](JSONValue model) {
			auto id = model.GetMember("id");
			if (!id.IsString()) {
				throw malformed();
			}
			if (!StringUtil::StartsWith(id.GetString(), "nace-ai/")) {
				return;
			}
			DrexListedModel listed {id.GetString()};
			auto modalities = model.GetMember("architecture").GetMember("input_modalities");
			if (modalities.IsArray()) {
				modalities.IterateArray([&](JSONValue modality) {
					listed.reads_images =
					    listed.reads_images || (modality.IsString() && modality.GetString() == "image");
				});
			}
			list->models.push_back(std::move(listed));
		});
	} else {
		throw malformed();
	}
	return list;
}

//! The listed model `model` is sent as, from GET /v1/models. Drex answers any unregistered drex-* name with drex-v2.0,
//! so only this check refuses one. The list is cached per instance and base URL, and fetched again for a model it
//! lacks, so a model released since the last fetch is found
static DrexListedModel FindModel(const string &function, ClientContext &context, const string &model) {
	auto client = WithFunctionName(function, [&]() { return DrexClient::Get(context); });
	auto &cache = ObjectCache::GetObjectCache(context);
	auto key = DrexModelList::ObjectType() + ":" + client->Credentials().base_url;
	auto models = cache.Get<DrexModelList>(key);
	if (!models || !models->Find(model)) {
		models = FetchModels(function, *client, context);
		cache.Put(key, models);
	}
	auto listed = models->Find(model);
	if (!listed) {
		vector<string> names;
		for (auto &entry : models->models) {
			names.push_back(entry.name);
		}
		throw DrexError(function, "unknown model '%s'; GET /v1/models lists %s", model, StringUtil::Join(names, ", "));
	}
	return *listed;
}

//! The NULL a row returns when its request has no answer, logged with the reason (for example, a body over 4 MiB)
static Value LostRow(ClientContext &context, const string &function, const DrexResult &answer,
                     const LogicalType &type) {
	DUCKDB_LOG_WARNING(context, "%s: a row returned NULL: %s", function, answer.error);
	return Value(type);
}

//===--------------------------------------------------------------------===//
// prompt_drex
//===--------------------------------------------------------------------===//

//! What `input` is: text sent as a JSON string state, JSON sent as a structured state, or images
enum class DrexInputKind : uint8_t { TEXT, JSON, IMAGE, IMAGES };

struct PromptDrexBindData : public FunctionData {
	string model;
	optional_idx concurrency;
	//! The position of `input` among the function's arguments
	idx_t input_index = 0;
	DrexInputKind input_kind = DrexInputKind::TEXT;
	vector<DrexQuestion> questions;
	//! The serialized `questions` of every request
	string questions_json;
	//! Whether the answer is one STRUCT field per question (questions :=), or the one question's answer itself
	bool named_answers = false;
	LogicalType return_type;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<PromptDrexBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<PromptDrexBindData>();
		return model == other.model && concurrency == other.concurrency && input_index == other.input_index &&
		       input_kind == other.input_kind && questions_json == other.questions_json &&
		       named_answers == other.named_answers && return_type == other.return_type;
	}
};

static DrexInputKind InputKind(const LogicalType &type) {
	if (type.IsJSONType()) {
		return DrexInputKind::JSON;
	}
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::SQLNULL:
		return DrexInputKind::TEXT;
	case LogicalTypeId::BLOB:
		return DrexInputKind::IMAGE;
	case LogicalTypeId::LIST:
		if (ListType::GetChildType(type).id() == LogicalTypeId::BLOB) {
			return DrexInputKind::IMAGES;
		}
		break;
	case LogicalTypeId::UNKNOWN:
		// A prepared parameter: DuckDB binds again when the statement runs, with the value's type
		throw ParameterNotResolvedException();
	default:
		break;
	}
	throw DrexError("prompt_drex", "input must be VARCHAR, JSON, BLOB or BLOB[], not %s", type.ToString());
}

static unique_ptr<FunctionData> PromptDrexBind(BindScalarFunctionInput &input) {
	const string function = "prompt_drex";
	for (auto &unsupported : {"multiple_choice", "box"}) {
		if (!input.GetConstant(Identifier(unsupported)).IsNull()) {
			throw DrexError(function, "%s is not supported, since Drex has no %s question", unsupported, unsupported);
		}
	}

	// As in prompt_jev, noul, choice, score and questions are mutually exclusive
	vector<string> given;
	for (auto &mode : {"noul", "choice", "score", "questions"}) {
		if (!input.GetConstant(Identifier(mode)).IsNull()) {
			given.push_back(mode);
		}
	}
	if (given.size() > 1) {
		throw DrexError(function, "%s are mutually exclusive; give one of noul, choice, score or questions",
		                StringUtil::Join(given, " and "));
	}

	auto result = make_uniq<PromptDrexBindData>();
	auto instructions = input.GetConstant(Identifier("instructions"));
	auto instructions_text = instructions.IsNull() ? string() : StringValue::Get(instructions);
	auto model = input.GetConstant(Identifier("model"));
	result->model = model.IsNull() ? DEFAULT_MODEL : StringValue::Get(model);
	auto concurrency = input.GetConstant(Identifier("concurrency"));
	if (!concurrency.IsNull()) {
		auto limit = UBigIntValue::Get(concurrency);
		if (limit == 0) {
			throw DrexError(function, "concurrency must be at least 1");
		}
		result->concurrency = limit;
	}

	auto mode = given.empty() ? string("noul") : given[0];
	if (mode == "questions") {
		if (!instructions_text.empty()) {
			throw DrexError(function, "with questions, give each question its own instructions");
		}
		auto questions = input.GetConstant(Identifier("questions"));
		auto &type = questions.type();
		if (type.IsJSONType() || type.id() == LogicalTypeId::VARCHAR) {
			result->questions = QuestionsFromJSON(function, StringValue::Get(questions));
		} else if (type.id() == LogicalTypeId::STRUCT) {
			result->questions = QuestionsFromStruct(function, questions);
		} else {
			throw DrexError(function, "questions must be a STRUCT or a JSON object of questions, not %s",
			                type.ToString());
		}
		result->named_answers = true;
	} else {
		DrexQuestion question;
		question.id = mode;
		question.type = ParseQuestionType(function, mode, mode);
		question.instructions = instructions_text;
		auto criteria = input.GetConstant(Identifier(mode));
		if (!criteria.IsNull()) {
			question.labels = LabelsFromValue(function, mode, criteria);
		}
		result->questions.push_back(std::move(question));
	}

	if (result->questions.empty() || result->questions.size() > MAX_QUESTIONS) {
		throw DrexError(function, "questions takes 1 to %llu questions, got %llu", MAX_QUESTIONS,
		                result->questions.size());
	}
	case_insensitive_set_t ids;
	child_list_t<LogicalType> answer_types;
	for (auto &question : result->questions) {
		auto name = result->named_answers ? "questions." + question.id : question.id;
		auto trimmed_id = question.id;
		StringUtil::Trim(trimmed_id);
		if (trimmed_id.empty()) {
			throw DrexError(function, "question ids must not be empty");
		}
		// Drex collects answers in a plain object, whose __proto__ is not a key, so it would bill the request and drop
		// the answer
		if (question.id == "__proto__") {
			throw DrexError(function, "the question id '__proto__' is not allowed: Drex can't answer it");
		}
		// The answers are fields of a STRUCT, whose names ignore case
		if (!ids.insert(question.id).second) {
			throw DrexError(function, "question ids must be unique ignoring case; '%s' is a duplicate", question.id);
		}
		CheckLabels(function, name, question);
		CheckNoulHasText(function, name, question);
		answer_types.emplace_back(Identifier(question.id), AnswerType(question.type));
	}
	result->questions_json = QuestionsToJSON(result->questions);
	result->return_type =
	    result->named_answers ? LogicalType::STRUCT(std::move(answer_types)) : AnswerType(result->questions[0].type);

	auto &arguments = input.GetArguments();
	auto names = input.GetArgumentNames();
	if (names) {
		for (idx_t i = 0; i < names->size(); i++) {
			if ((*names)[i] == "input") {
				result->input_index = i;
			}
		}
	}
	result->input_kind = InputKind(arguments[result->input_index]->GetReturnType());

	// Last, since it may call GET /v1/models
	bool reads_images = FindModel(function, input.GetClientContext(), result->model).reads_images;
	bool has_images = result->input_kind == DrexInputKind::IMAGE || result->input_kind == DrexInputKind::IMAGES;
	if (has_images && !reads_images) {
		throw DrexError(function, "%s does not read images; use an image model such as drex-max", result->model);
	}

	input.GetBoundFunction().SetReturnType(result->return_type);
	return std::move(result);
}

//! Serializes text as a JSON string
static string JSONString(const string &text) {
	JSONWriter writer;
	writer.SetRoot(writer.CreateString(text));
	return writer.ToString();
}

//! The request for one row's input, or false when the row sends nothing: a NULL input, a NULL image, or no images
static bool MakeRequest(const PromptDrexBindData &bind, const Value &input, DrexRequest &request) {
	if (input.IsNull()) {
		return false;
	}
	request.questions = bind.questions_json;
	switch (bind.input_kind) {
	case DrexInputKind::TEXT:
		request.state = JSONString(StringValue::Get(input));
		return true;
	case DrexInputKind::JSON:
		request.state = StringValue::Get(input);
		return true;
	case DrexInputKind::IMAGE:
		request.state = JSONString("");
		request.images.push_back(StringValue::Get(input));
		return true;
	case DrexInputKind::IMAGES:
		request.state = JSONString("");
		for (auto &image : ListValue::GetChildren(input)) {
			if (image.IsNull()) {
				return false;
			}
			request.images.push_back(StringValue::Get(image));
		}
		return !request.images.empty();
	}
	throw InternalException("prompt_drex: unknown input kind");
}

static Value ResultValue(const PromptDrexBindData &bind, const string &json) {
	const string function = "prompt_drex";
	JSONParseError parse_error;
	auto document = JSONDocument::TryParse(json.c_str(), json.size(), parse_error);
	if (!document) {
		throw IOException("%s: Drex answered with a body that is not JSON", function);
	}
	auto answers = document->GetRoot().GetMember("answers");
	if (!bind.named_answers) {
		auto &question = bind.questions[0];
		return AnswerValue(function, question, MemberOf(answers, question.id));
	}
	vector<Value> values;
	for (auto &question : bind.questions) {
		values.push_back(AnswerValue(function, question, MemberOf(answers, question.id)));
	}
	return Value::STRUCT(bind.return_type, std::move(values));
}

static void PromptDrexFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<PromptDrexBindData>();
	auto &input = args.data[bind.input_index];

	vector<DrexRequest> requests;
	vector<idx_t> rows;
	for (idx_t row = 0; row < args.size(); row++) {
		DrexRequest request;
		if (MakeRequest(bind, input.GetValue(row), request)) {
			requests.push_back(std::move(request));
			rows.push_back(row);
		} else {
			result.SetValue(row, Value(bind.return_type));
		}
	}
	if (requests.empty()) {
		return;
	}
	auto answers = WithFunctionName("prompt_drex", [&]() {
		auto &context = state.GetContext();
		return DrexClient::Get(context)->DecideAll(bind.model, requests, bind.concurrency, "prompt_drex", &context);
	});
	if (answers.size() != requests.size()) {
		throw InternalException("prompt_drex: %llu requests got %llu results", requests.size(), answers.size());
	}
	for (idx_t i = 0; i < rows.size(); i++) {
		auto &answer = answers[i];
		result.SetValue(rows[i], answer.Success()
		                             ? ResultValue(bind, answer.json)
		                             : LostRow(state.GetContext(), "prompt_drex", answer, bind.return_type));
	}
}

//===--------------------------------------------------------------------===//
// drex_decide
//===--------------------------------------------------------------------===//

struct DrexDecideBindData : public FunctionData {
	string model;

	explicit DrexDecideBindData(string model) : model(std::move(model)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DrexDecideBindData>(model);
	}
	bool Equals(const FunctionData &other) const override {
		return model == other.Cast<DrexDecideBindData>().model;
	}
};

static unique_ptr<FunctionData> DrexDecideBind(BindScalarFunctionInput &input) {
	auto model = input.GetConstant(Identifier("model"));
	auto result = make_uniq<DrexDecideBindData>(model.IsNull() ? DEFAULT_MODEL : StringValue::Get(model));
	FindModel("drex_decide", input.GetClientContext(), result->model);
	return std::move(result);
}

//! Sends state and questions as written and returns Drex's response body as written
static void DrexDecideFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<DrexDecideBindData>();

	vector<DrexRequest> requests;
	vector<idx_t> rows;
	for (idx_t row = 0; row < args.size(); row++) {
		auto request_state = args.data[0].GetValue(row);
		auto questions = args.data[1].GetValue(row);
		if (request_state.IsNull() || questions.IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		DrexRequest request;
		request.state = StringValue::Get(request_state);
		request.questions = StringValue::Get(questions);
		requests.push_back(std::move(request));
		rows.push_back(row);
	}
	if (requests.empty()) {
		return;
	}
	auto answers = WithFunctionName("drex_decide", [&]() {
		auto &context = state.GetContext();
		return DrexClient::Get(context)->DecideAll(bind.model, requests, optional_idx(), "drex_decide", &context);
	});
	if (answers.size() != requests.size()) {
		throw InternalException("drex_decide: %llu requests got %llu results", requests.size(), answers.size());
	}
	for (idx_t i = 0; i < rows.size(); i++) {
		auto &answer = answers[i];
		result.SetValue(rows[i], answer.Success()
		                             ? Value(answer.json)
		                             : LostRow(state.GetContext(), "drex_decide", answer, result.GetType()));
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

//! Every call is a billed request, so neither function is ever folded or deduplicated
static void SetRequestProperties(ScalarFunction &function) {
	function.SetVolatile();
	function.SetFallible();
}

//! prompt_drex(input, instructions [, noul|choice|score := criteria] [, questions :=] [, model :=] [, concurrency :=])
//! As in prompt_jev, the criteria of noul, choice and score are a VARCHAR[] of labels or a
//! STRUCT(label VARCHAR, description VARCHAR)[]
//! Keyword-only parameters, since DuckDB checks their names and types (it does not for typed **kwargs on scalar
//! functions). An omitted one reaches the bind callback as NULL, so the function handles NULLs itself: under the
//! default handling a NULL default would fold every call to NULL. The bind callback resolves the return type
static ScalarFunction PromptDrexFunctionDefinition() {
	FunctionSignature signature;
	signature.AddPositionalOnly("input", LogicalType::ANY);
	signature.AddParameter("instructions", LogicalType::VARCHAR, Value(""));
	signature.AddKeywordOnly("noul", LogicalType::ANY, Value());
	signature.AddKeywordOnly("choice", LogicalType::ANY, Value());
	signature.AddKeywordOnly("score", LogicalType::ANY, Value());
	signature.AddKeywordOnly("questions", LogicalType::ANY, Value());
	// prompt_jev's, declared so that the bind callback can refuse them by name
	signature.AddKeywordOnly("multiple_choice", LogicalType::ANY, Value());
	signature.AddKeywordOnly("box", LogicalType::ANY, Value());
	signature.AddKeywordOnly("model", LogicalType::VARCHAR, Value(DEFAULT_MODEL));
	signature.AddKeywordOnly("concurrency", LogicalType::UBIGINT, Value(LogicalType::UBIGINT));
	signature.SetReturnType(LogicalType::ANY);
	ScalarFunction function("prompt_drex", std::move(signature), PromptDrexFunction);
	function.SetBindCallback(PromptDrexBind);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	SetRequestProperties(function);
	return function;
}

//! drex_decide(state, questions [, model :=]) -> JSON: the request and response bodies as written
static ScalarFunction DrexDecideFunctionDefinition() {
	FunctionSignature signature;
	signature.AddPositionalOnly("state", LogicalType::JSON());
	signature.AddPositionalOnly("questions", LogicalType::JSON());
	signature.AddKeywordOnly("model", LogicalType::VARCHAR, Value(DEFAULT_MODEL));
	signature.SetReturnType(LogicalType::JSON());
	ScalarFunction function("drex_decide", std::move(signature), DrexDecideFunction);
	function.SetBindCallback(DrexDecideBind);
	SetRequestProperties(function);
	return function;
}

void RegisterDrexFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(PromptDrexFunctionDefinition());
	loader.RegisterFunction(DrexDecideFunctionDefinition());
}

} // namespace duckdb
