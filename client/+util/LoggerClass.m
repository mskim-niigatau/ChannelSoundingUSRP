classdef LoggerClass < handle
    % Logger - A simple logger class that displays messages to the command window and writes them to a log file.
    % USAGE: logger = LoggerClass(args)
    %   args - A struct with the following fields:
    %       writeToFile - Whether to write the log messages to a file. The default is false.
    %       fileName - The file to write the log messages to. The default is a temporary file in the system's temp directory.
    %       timeFormat - The format to use for the time in the log messages. The default is "HH:mm:ss.SS".
    %       logLevel - The log level to use. The default is 1 (DEBUG).
    %           1 - DEBUG
    %           2 - INFO
    %           3 - WARNING
    %           4 - ERROR
    %           5 - CRITICAL
    %
    %   Properties:
    %       fileName - The file to write the log messages to.
    %       timeFormat - The format to use for the time in the log messages.
    %       logLevel - The log level to use.
    %
    %   Methods:
    %       setlevel - Set the log level.
    %       log - Log a message.
    %       debug - Log a debug message.
    %       info - Log an info message.
    %       warning - Log a warning message.
    %       error - Log an error message.
    %       critical - Log a critical message.
    %
    %   Example:
    %       logger = Logger();
    %       logger.log(1, "This is a debug message.");
    %       logger.log(2, "This is an info message.");
    %       logger.log(3, "This is a warning message.");
    %       logger.log(4, "This is an error message.");
    %       logger.log(5, "This is a critical message.");
    %

    properties (GetAccess = public, SetAccess = protected)
        fileName = '' % The file to write the log messages to.
        timeFormat = '' % The format to use for the time in the log messages.
        logLevel = 1 % The log level to use.
    end

    properties (Access = private)
        fileHandle = []
    end

    methods (Access = private)

        function time = gettimenow(obj)
            % gettimenow - Get the current time as a string.
            %
            %   Outputs:
            %       time - The current time as a string.
            time = datetime("now", "format", obj.timeFormat);
        end

        function formattedMessage = formatlogmessage(obj, message, level)
            % formatlogmessage - Format a log message.
            %
            %   Inputs:
            %       message - The message to log.
            %       level - The log level of the message.
            %
            %   Outputs:
            %       formattedMessage - The formatted message.
            formattedMessage = sprintf("[%s] [%s] %s", obj.gettimenow(), obj.getloglevelstring(level), message);
        end

        function logLevelString = getloglevelstring(~, level)
            % getloglevelstring - Get the log level string.
            %
            %   Inputs:
            %       level - The log level.
            %
            %   Outputs:
            %       logLevelString - The log level string.
            switch level
                case 1
                    logLevelString = "DEBUG";
                case 2
                    logLevelString = "INFO";
                case 3
                    logLevelString = "WARNING";
                case 4
                    logLevelString = "ERROR";
                case 5
                    logLevelString = "CRITICAL";
                otherwise
                    logLevelString = "UNKNOWN";
            end

        end

    end

    methods (Access = public)

        function obj = LoggerClass(args)
            % Logger - Constructor for the Logger class.
            %
            %   Inputs:
            %       writeToFile - Whether to write the log messages to a file. The default is false.
            %       fileName - The file to write the log messages to.
            %       timeFormat - The format to use for the time in the log messages. The default is "HH:mm:ss.SS".
            %       logLevel - The log level to use. The default is 1 (DEBUG).
            %           1 - DEBUG
            %           2 - INFO
            %           3 - WARNING
            %           4 - ERROR
            %           5 - CRITICAL
            %
            arguments
                args.writeToFile (1, 1) {mustBeNumericOrLogical} = false
                args.fileName (1, 1) string = [tempname '.log']
                args.timeFormat (1, 1) string = "HH:mm:ss.SS"
                args.logLevel (1, 1) {mustBeInteger, mustBeInRange(args.logLevel, 1, 5)} = 1
            end

            obj.fileName = args.fileName;
            obj.logLevel = args.logLevel;
            obj.timeFormat = args.timeFormat;

            % Create the log file.
            if args.writeToFile
                obj.fileHandle = fopen(obj.fileName, 'w');

                if obj.fileHandle == -1
                    error("Failed to create log file: %s", obj.fileName);
                end

            end

        end

        function setlevel(obj, level)
            % setlevel - Set the log level.
            %
            %   Inputs:
            %       level - The log level.
            if isstring(level) || ischar(level)

                switch level
                    case "DEBUG"
                        level = 1;
                    case "INFO"
                        level = 2;
                    case "WARNING"
                        level = 3;
                    case "ERROR"
                        level = 4;
                    case "CRITICAL"
                        level = 5;
                    otherwise
                        error("Invalid log level: %s", level);
                end

            end

            obj.logLevel = level;
        end

        function log(obj, level, message, varargin)
            % log - Log a message.
            %
            %   Inputs:
            %       level - The log level of the message.
            %       message - The message to log.
            %       varargin - The arguments to the message.
            if level < obj.logLevel
                return;
            end

            % if arguments number is greater than 3, then the message is a format string
            if nargin > 3
                message = sprintf(message, varargin{:});
            end

            formattedMessage = obj.formatlogmessage(message, level);

            if ~isempty(obj.fileHandle)
                fprintf(obj.fileHandle, "%s\n", formattedMessage);
            end

            fprintf("%s\n", formattedMessage);
        end

        function debug(obj, message, varargin)
            % debug - Log a debug message.
            %
            %   Inputs:
            %       message - The message to log.
            %       varargin - The arguments to the message.
            obj.log(1, message, varargin{:});
        end

        function info(obj, message, varargin)
            % info - Log an info message.
            %
            %   Inputs:
            %       message - The message to log.
            %      varargin - The arguments to the message.
            obj.log(2, message, varargin{:});
        end

        function warning(obj, message, varargin)
            % warning - Log a warning message.
            %
            %   Inputs:
            %       message - The message to log.
            %       varargin - The arguments to the message.
            obj.log(3, message, varargin{:});
        end

        function error(obj, message, varargin)
            % error - Log an error message.
            %
            %   Inputs:
            %       message - The message to log.
            %       varargin - The arguments to the message.
            obj.log(4, message, varargin{:});
        end

        function critical(obj, message, varargin)
            % critical - Log a critical message.
            %
            %   Inputs:
            %       message - The message to log.
            %       varargin - The arguments to the message.
            obj.log(5, message, varargin{:});
        end

        function delete(obj)
            % delete - Destructor for the Logger class.
            if ~isempty(obj.fileHandle)
                fclose(obj.fileHandle);
                fprintf("Log file: %s\n", obj.fileName);
            end

        end

    end

end
