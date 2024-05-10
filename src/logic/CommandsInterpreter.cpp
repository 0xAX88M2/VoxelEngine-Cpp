#include "CommandsInterpreter.hpp"

#include "../coders/commons.hpp"
#include "../util/stringutil.hpp"

#include <iostream>

using namespace cmd;

inline bool is_cmd_identifier_part(char c, bool allowColon) {
    return is_identifier_part(c) || c == '.' || c == '$' || c == '@' || 
           (allowColon && c == ':');
}

inline bool is_cmd_identifier_start(char c) {
    return is_identifier_start(c) || c == '.' || c == '$' || c == '@';
}

class CommandParser : BasicParser {
    std::string parseIdentifier(bool allowColon) {
        char c = peek();
        if (!is_identifier_start(c) && c != '$') {
            if (c == '"') {
                pos++;
                return parseString(c);
            }
            throw error("identifier expected");
        }
        int start = pos;
        while (hasNext() && is_cmd_identifier_part(source[pos], allowColon)) {
            pos++;
        }
        return std::string(source.substr(start, pos-start));
    }

    std::unordered_map<std::string, ArgType> types {
        {"num", ArgType::number},
        {"int", ArgType::integer},
        {"str", ArgType::string},
        {"@", ArgType::selector},
        {"enum", ArgType::enumvalue},
    };
public:
    CommandParser(std::string_view filename, std::string_view source) 
    : BasicParser(filename, source) {
    }

    ArgType parseType() {
        if (peek() == '[') {
            return ArgType::enumvalue;
        }
        std::string name = parseIdentifier(false);
        auto found = types.find(name);
        if (found != types.end()) {
            return found->second;
        } else {
            throw error("unknown type "+util::quote(name));
        }
    }

    dynamic::Value parseValue() {
        char c = peek();
        if (is_cmd_identifier_start(c)) {
            auto str = parseIdentifier(true);
            if (str == "true") {
                return true;
            } else if (str == "false") {
                return false;
            } else if (str == "none" || str == "nil" || str == "null") {
                return dynamic::NONE;
            }
            return str;
        }
        if (c == '"' || c == '\'') {
            nextChar();
            return parseString(c);
        }
        if (c == '+' || c == '-' || is_digit(c)) {
            return parseNumber(c == '-' ? -1 : 1);
        }
        throw error("invalid character '"+std::string({c})+"'");
    }

    std::string parseEnum() {
        if (peek() == '[') {
            nextChar();
            if (peek() == ']') {
                throw error("empty enumeration is not allowed");
            }
            auto enumvalue = "|"+std::string(readUntil(']'))+"|";
            size_t offset = enumvalue.find(' ');
            if (offset != std::string::npos) {
                goBack(enumvalue.length()-offset);
                throw error("use '|' as separator, not a space");
            }
            nextChar();
            return enumvalue;
        } else {
            expect('$');
            goBack();
            return parseIdentifier(false);
        }
    }

    Argument parseArgument() {
        std::string name = parseIdentifier(false);
        expect(':');
        ArgType type = parseType();
        std::string enumname = "";
        if (type == ArgType::enumvalue) {
            enumname = parseEnum();
        }
        bool optional = false;
        dynamic::Value def {};
        dynamic::Value origin {};
        bool loop = true;
        while (hasNext() && loop) {
            char c = peek();
            switch (c) {
                case '=':
                    nextChar();
                    optional = true;
                    def = parseValue();
                    break;
                case '~':
                    nextChar();
                    origin = parseValue();
                    break;
                default:
                    loop = false;
                    break;
            }
        }
        return Argument {name, type, optional, def, origin, enumname};
    }

    Command parseScheme(executor_func executor) {
        std::string name = parseIdentifier(true);
        std::vector<Argument> args;
        std::unordered_map<std::string, Argument> kwargs;
        while (hasNext()) {
            if (peek() == '{') {
                nextChar();
                while (peek() != '}') {
                    Argument arg = parseArgument();
                    kwargs[arg.name] = arg;
                }
                nextChar();
            } else {
                args.push_back(parseArgument());
            }
        }
        return Command(name, std::move(args), std::move(kwargs), executor);
    }

    inline parsing_error argumentError(
        const std::string& argname, 
        const std::string& message
    ) {
        return error("argument "+util::quote(argname)+": "+message);
    }

    inline parsing_error typeError(
        const std::string& argname, 
        const std::string& expected, 
        const dynamic::Value& value
    ) {
        return argumentError(
            argname, expected+" expected, got "+dynamic::type_name(value)
        );
    }

    template<typename T>
    bool typeCheck(Argument* arg, const dynamic::Value& value, const std::string& tname) {
        if (!std::holds_alternative<T>(value)) {
            if (arg->optional) {
                return false;
            } else {
                throw typeError(arg->name, tname, value);
            }
        }
        return true;
    }

    bool typeCheck(Argument* arg, const dynamic::Value& value) {
        switch (arg->type) {
            case ArgType::enumvalue: {
                if (auto* string = std::get_if<std::string>(&value)) {
                    auto& enumname = arg->enumname;
                    if (enumname.find("|"+*string+"|") == std::string::npos) {
                        throw error("argument "+util::quote(arg->name)+
                                    ": invalid enumeration value");
                    }
                } else {
                    if (arg->optional) {
                        return false;
                    }
                    throw typeError(arg->name, "enumeration value", value);
                }
                break;
            }
            case ArgType::number:
                if (!dynamic::is_numeric(value)) {
                    if (arg->optional) {
                        return false;
                    } else {
                        throw typeError(arg->name, "number", value);
                    }
                }
                break;
            case ArgType::integer:
                return typeCheck<integer_t>(arg, value, "integer");
            case ArgType::string:
                return typeCheck<std::string>(arg, value, "string");
            case ArgType::selector:
                return typeCheck<integer_t>(arg, value, "id");
        }
        return true;
    }

    dynamic::Value fetchOrigin(CommandsInterpreter* interpreter, Argument* arg) {
        if (dynamic::is_numeric(arg->origin)) {
            return arg->origin;
        } else if (auto string = std::get_if<std::string>(&arg->origin)) {
            return (*interpreter)[*string];
        }
        return dynamic::NONE;
    }

    dynamic::Value applyRelative(
        Argument* arg, 
        dynamic::Value value,
        dynamic::Value origin
    ) {
        if (origin.index() == 0) {
            return value;
        }
        try {
            if (arg->type == ArgType::number) {
                return dynamic::get_number(origin) + dynamic::get_number(value);
            } else {
                return dynamic::get_integer(origin) + dynamic::get_integer(value);
            }
        } catch (std::runtime_error& err) {
            throw argumentError(arg->name, err.what());
        }
    }

    dynamic::Value parseRelativeValue(CommandsInterpreter* interpreter, Argument* arg) {
        if (arg->type != ArgType::number && arg->type != ArgType::integer) {
            throw error("'~' operator is only allowed for numeric arguments");
        }
        nextChar();
        auto origin = fetchOrigin(interpreter, arg);
        if (peekNoJump() == ' ' || !hasNext()) {
            return origin;
        }
        auto value = parseValue();
        if (origin.index() == 0) {
            return value;
        }
        return applyRelative(arg, value, origin);
    }

    inline dynamic::Value performKeywordArg(
        CommandsInterpreter* interpreter, Command* command, const std::string& key
    ) {
        if (auto arg = command->getArgument(key)) {
            nextChar();
            auto value = peek() == '~' 
                ? parseRelativeValue(interpreter, arg) 
                : parseValue();
            typeCheck(arg, value);
            return value;
        } else {
            throw error("unknown keyword "+util::quote(key));
        }
    }

    Prompt parsePrompt(CommandsInterpreter* interpreter) {
        auto repo = interpreter->getRepository();
        std::string name = parseIdentifier(true);
        auto command = repo->get(name);
        if (command == nullptr) {
            throw error("unknown command "+util::quote(name));
        }
        auto args = dynamic::create_list();
        auto kwargs = dynamic::create_map();

        int arg_index = 0;

        while (hasNext()) {
            bool relative = false;
            dynamic::Value value = dynamic::NONE;
            if (peek() == '~') {
                relative = true;
                value = 0;
                nextChar();
            }
            
            if (hasNext() && peekNoJump() != ' ') {
                value = parseValue();

                // keyword argument
                if (!relative && hasNext() && peek() == '=') {
                    auto key = std::get<std::string>(value);
                    kwargs->put(key, performKeywordArg(interpreter, command, key));
                }
            }

            // positional argument
            Argument* arg;
            do {
                arg = command->getArgument(arg_index++);
                if (arg == nullptr) {
                    throw error("extra positional argument");
                }
                if (arg->origin.index() && relative) {
                    break;
                }
            } while (!typeCheck(arg, value));

            if (relative) {
                value = applyRelative(arg, value, fetchOrigin(interpreter, arg));
            }
            args->put(value);
        }

        while (auto arg = command->getArgument(arg_index++)) {
            if (!arg->optional) {
                throw error("missing argument "+util::quote(arg->name));
            }
        }
        return Prompt {command, args, kwargs};
    }
};

Command Command::create(std::string_view scheme, executor_func executor) {
    return CommandParser("<string>", scheme).parseScheme(executor);
}

void CommandsRepository::add(std::string_view scheme, executor_func executor) {
    Command command = Command::create(scheme, executor);
    commands[command.getName()] = command;
}

Command* CommandsRepository::get(const std::string& name) {
    auto found = commands.find(name);
    if (found == commands.end()) {
        return nullptr;
    }
    return &found->second;
}

Prompt CommandsInterpreter::parse(std::string_view text) {
    return CommandParser("<string>", text).parsePrompt(this);
}
