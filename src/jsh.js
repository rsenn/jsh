// var jsh, global, __filename, require, process;

var rl = require('ReadLine');
var pc = require('ProcessChain');
var Job = require('Job');
var Completion = require('Completion');
var Tokenizer = require('Tokenizer');
var jshnative = require('jsh');
var path = require('path');
var fs = require('fs');
//var Service = require('Service');
var ifsOverrideStack = [];
jsh = {
  get IFS() {
    return ifsOverrideStack.length ? ifsOverrideStack[ifsOverrideStack.length - 1] : '\n';
  },
  path: /^(.*\/)[^/]*$/.exec(__filename)[1],
  jshNative: new jshnative.jsh(),
  Job: Job,
  jobCount: 0,
  completion: new Completion.Completion(),
  pathify: function(prog) {
    if(prog.indexOf('/') == -1) {
      // check PATH
      var path = global.PATH;
      if(typeof path !== 'string') {
        // throw here?
        return '';
      }
      // split on ':'
      path = path.split(':');
      for(var i in path) {
        if(jsh.jshNative.isExecutable(path[i] + '/' + prog)) prog = path[i] + '/' + prog;
      }
      if(prog.indexOf('/') == -1) {
        throw 'File not found: ' + prog;
      }
    } else if(!jsh.jshNative.isExecutable(prog)) {
      throw 'File not found: ' + prog;
    }

    return prog;
  },
  config: {
    logEnabled: false,
    expandVariables: true,
    prettyReturnValues: 4,
    printUndefinedReturn: false
  },
  log: function() {
    if(jsh.config.logEnabled) console.log.apply(console, arguments);
  },
  error: function() {
    if(jsh.config.logEnabled) console.error.apply(console, arguments);
  },
  promptIdx: 0,
  prompt: function() {
    if(typeof this._userPrompt === 'function') {
      try {
        return this._userPrompt();
      } catch(e) {
        console.error('prompt error: ' + e);
      }
    }
    var p = 'jsh(' + ++this.promptIdx + '): ';
    return p;
  },
  setPrompt: function(p) {
    this._userPrompt = p;
  },
  execSync: function(cmd, args) {
    return this.jshNative.execSync(this.pathify(cmd), args);
  }
};
jsh.jshNative.setupShell();
var read;
var runState;

function RunState() {
  this._data = [];
}

RunState.prototype.push = function(cb) {
  this._data.push({ cb: cb, status: [] });
};

RunState.prototype.pop = function() {
  var data = this._data.pop();
  var c = this._calc(data.status);
  jsh.log('popping with ' + JSON.stringify(c));
  data.cb(c.status);
};

RunState.prototype.at = function(pos) {
  if(pos < 0 || pos >= this._data.length) return undefined;
  return this._data[pos];
};

RunState.prototype.update = function(status) {
  this._data[this._data.length - 1].status = [status];
};

RunState.prototype.checkOperator = function(op, ret) {
  var cur = this._data[this._data.length - 1];
  if(op === '|') return true;
  if(op === '&&' || op === '||' || op === ';' || op === '&') {
    switch (this._currentOp()) {
      case ';':
      case '|':
        cur.status = [];
        break;
    }
    cur.status.push(ret, op);
    return this._calc(cur.status).cont;
  }
  return false;
};

RunState.prototype._currentOp = function() {
  if(this._data.length === 0) return '';
  var cur = this._data[this._data.length - 1];
  if(cur.status.length < 2) return '';
  var pos = cur.status.length - 1;
  if(!(pos % 2)) return '';
  return cur.status[pos];
};

RunState.prototype._calc = function(status) {
  if(status.length === 0) return { cont: true, status: true };
  var cur = undefined;
  var op = undefined;
  for(var idx = 0; idx < status.length; ++idx) {
    if(cur === undefined) {
      cur = status[idx];
      continue;
    }
    if(idx % 2) {
      op = status[idx];
      var next = idx + 1 < status.length ? status[idx + 1] : undefined;

      switch (op) {
        case '&&':
          if(!cur) return { cont: false, status: false };
          if(next !== undefined) {
            cur = next;
          }
          break;
        case '||':
          if(cur) return { cont: false, status: true };
          if(next !== undefined) {
            cur = next;
          }
          break;
        case ';':
          return { cont: true, status: cur };
      }
    }
  }
  return { cont: true, status: cur === undefined ? true : cur };
};

function isFunction(token) {
  if(token[0].type === Tokenizer.COMMAND) {
    // Check if the first token is an existing function

    var list = token[0].data.split('.');
    var obj = global;
    for(var i in list) {
      if(obj === undefined) return false;
      jsh.log('testing ' + list[i]);
      obj = obj[list[i]];
    }
    return typeof obj === 'function';
  }
  return false;
}

function maybeJavaScript(token) {
  if(token[0].type === Tokenizer.GROUP) {
    return false;
  } else if(token[0].type === Tokenizer.JAVASCRIPT) {
    if(token.length !== 1) {
      throw 'Unexpected JS token length: ' + token.length;
    }
    return true;
  } else if(isFunction(token)) {
    return true;
  }
  return false;
}

function runJavaScript(token, job) {
  var func = '';
  var state = 0;
  var cnt = 0,
    i;

  if(token.length < 1) {
    throw 'Token length < 1 - ' + token.length;
  }
  var end = token.length;
  if(
    token[token.length - 1].type === Tokenizer.OPERATOR ||
    token[token.length - 1].type === Tokenizer.HIDDEN
  ) {
    end = token.length - 1;
  }

  if(token[0].type !== Tokenizer.JAVASCRIPT) {
    for(i = 0; i < end; ++i) {
      if(!func) {
        func = token[i].data + '(';
      } else {
        if(token[i].type === Tokenizer.GROUP) {
          func += token[i].data;
        } else {
          if(token[i].data === "'") {
            if(state === 0) {
              if(cnt) func += ', ';
              func += "'";
              state = 1;
            } else {
              func += "'";
              state = 0;
            }
          } else {
            if(state === 0 && cnt) func += ', ';
            if(token[i].type === Tokenizer.VARIABLE) func += token[i].data;
            else if(!state) func += "'" + token[i].data + "'";
            else func += token[i].data;
          }
          ++cnt;
        }
      }
    }
    func += ')';
  } else {
    for(i in token) {
      func += token[i].data + ' ';
    }
  }

  if(job) {
    var jobfunc = eval('(function*() {' + func + '})');
    jsh.log('creating func', func, jobfunc, typeof jobfunc);
    job.js(new Job.JavaScript(jobfunc));
    return undefined;
  } else {
    jsh.log('evaling ' + func);
    return eval.call(global, func);
  }
}

function operator(token) {
  if(token.length === 0) return undefined;
  var tok = token[token.length - 1];
  if(tok.type === Tokenizer.OPERATOR) return tok.data;
  else if(tok.type === Tokenizer.HIDDEN && tok.data === ';') return tok.data;
  return undefined;
}

function hasWait(obj) {
  if(typeof obj === 'object') if (typeof obj.jsh === 'object') return obj.jsh.wait;
  return false;
}

function jsReturn(ret) {
  if(typeof ret === 'boolean') return ret;
  if(typeof ret === 'object') if (typeof ret.jsh === 'object') return ret.jsh.ret;
  return !!ret;
}

function runTokens(tokens, pos) {
  if(pos === tokens.length) {
    runState.pop();
    return;
  }

  var job, j;
  for(var i = pos; i < tokens.length; ++i) {
    var token = tokens[i];
    var op = operator(token);
    jsh.log('---- ' + i + ' ' + pos + ' ' + tokens.length);
    op = operator(token);
    if(op === undefined) {
      throw 'Unrecognized operator: ' + JSON.stringify(token, null, 4);
    }
    // remove the operator
    token.pop();

    jsh.log('operator ' + op);
    if(op === '|') {
      if(!job) {
        job = new Job.Job();
      }
    } else if(op !== ';' && job) {
      throw 'Invalid operator for pipe job';
    }
    if(jsh.config.logEnabled) {
      for(j = 0; j < token.length; ++j) {
        jsh.log('  token ' + Tokenizer.tokenName(token[j].type) + " '" + token[j].data + "'");
      }
    }

    var iscmd = true,
      ret;
    if(token.length >= 1 && token[0].type === Tokenizer.GROUP) {
      jsh.log('    is a group');
      // run the group
      runState.push(function(ret) {
        if(runState.checkOperator(op, ret)) runTokens(tokens, pos + 1);
        else runState.pop();
      });
      runLine(token[0].data);
      return;
    } else if(maybeJavaScript(token)) {
      jsh.log('    might be js');
      iscmd = false;
      try {
        ret = runJavaScript(token, job);
      } catch(e) {
        console.error(e);
        runState.pop();
        return;
      }
    }
    if(!iscmd) {
      if(hasWait(ret)) {
        jsh.log('pushing...');
        runState.push(function(ret) {
          jsh.log('done!');
          if(runState.checkOperator(op, ret)) runTokens(tokens, pos + 1);
          else runState.pop();
        });
        return;
      }
      if(runState.checkOperator(op, jsReturn(ret))) {
        continue;
      } else {
        runState.pop();
        return;
      }
    }
    jsh.log('  is a command');
    var cmd = undefined;
    var args = [];
    for(j = 0; j < token.length; ++j) {
      if(cmd === undefined) {
        cmd = token[j].data;
      } else if(token[j].type !== Tokenizer.HIDDEN) {
        args.push(token[j].data);
      }
    }
    if(cmd !== undefined) {
      jsh.log('execing cmd ' + cmd);
      try {
        if(job) {
          job.proc({
            program: cmd,
            arguments: args,
            environment: jsh.environment(),
            cwd: process.cwd()
          });
        } else {
          var procjob = new Job.Job();
          procjob.proc({
            program: cmd,
            arguments: args,
            environment: jsh.environment(),
            cwd: process.cwd()
          });
          procjob.exec(
            Job.FOREGROUND,
            function(arg) {
              jsh.jshNative.stdout(arg);
            },
            function(code) {
              if(procjob.type === Job.BACKGROUND) return;
              if(runState.checkOperator(op, !code)) {
                try {
                  runTokens(tokens, pos + 1, runState);
                } catch(e) {
                  console.log('e1 ' + e);
                  runState.pop();
                }
              } else {
                runState.pop();
              }
            }
          );
          return;
        }
      } catch(e) {
        console.log('e2 ' + e);
        throw e;
      }
    }
  }
  if(job) {
    jsh.log('running job');
    job.exec(
      Job.FOREGROUND,
      function(arg) {
        jsh.jshNative.stdout(arg);
      },
      function(code) {
        if(job.type === Job.FOREGROUND) {
          runState.update(!code);
          runState.pop();
        }
      }
    );
  }
}

function isJSError(e) {
  return e instanceof SyntaxError || e instanceof ReferenceError;
}

function replaceVariables(line, tokens) {
  var out = line;
  var off = 0;
  for(var idx = 0; idx < tokens.length; ++idx) {
    var token = tokens[idx];
    for(var sub = 0; sub < token.length; ++sub) {
      var entry = token[sub];
      if(entry.type === Tokenizer.VARIABLE) {
        var from = entry.from - 1; // include the $
        var rep = '' + eval(entry.data);
        var prev = out.substring(0, off + from);
        var next = out.substring(off + entry.to);
        out = prev + rep + next;
        off += rep.length - (entry.to - from);
      }
    }
  }
  return out;
}

function runLine(line) {
  var tokens = [];
  var tok = new Tokenizer.Tokenizer(Tokenizer.SHELL);
  tok.tokenize(line);
  var commands = [];
  var command;
  while((command = tok.next())) {
    commands.push(command);
  }
  runCommands(commands, line);
}

function runCommands(commands, line) {
  // console.log("------------------------RUNCOMMANDS\n", line, "\n", commands);
  for(var c = 0; c < commands.length; ++c) {
    var command = commands[c];
    for(var idx = 0; idx < command.length; ++idx) {
      jsh.log(c + '/' + commands.length + ' ' + command[idx].type + ' -> ' + command[idx].data);
    }
    for(var i = 0; i < command.length; ++i) {
      // console.log("RUNNING TOKEN", i, command[i]);
      if(command[i].type === Tokenizer.EXECUTE) {
        if(i == 0 || i + 1 >= command.length) throw 'Something wrong, execute not surrounded by `';
        var oldOut = jsh.jshNative.stdout;
        var subCommandData = '';
        jsh.jshNative.stdout = function(data) {
          subCommandData += data;
        };

        command[i].type = Tokenizer.COMMAND;
        // console.log("FISKEFAEN", commands);
        var continueCommands = true;
        runState.push(function() {
          jsh.jshNative.stdout = oldOut;
          if(continueCommands) {
            var split = subCommandData.split(/\s+/);
            if(split[0] === '') split.splice(0, 1);
            if(split[split.length - 1] === '') split.splice(split.length - 1, 1);
            var res = split.join(' ');
            jsh.log("Replaced subshell output '" + command[i].data + "' => '" + res + "'");
            command[i].data = res;
            command.splice(i + 1, 1);
            command.splice(i - 1, 1);
            if(i > 1) {
              var prev = command[i - 2];
              if(prev.type === Tokenizer.COMMAND && prev.to + 1 === command[i - 1].from) {
                // join commands
                prev.data += command[i - 1].data;
                command.splice(i - 1, 1);
              }
            }
            // console.log("SSSSSSSSSS", command[i - 2], command[i - 1]);
            // console.log("FISK", commands);
            runCommands(commands, line);
          }
        });

        try {
          // console.log("RUNNING SUBSHELL", command[i].data);
          runLine(command[i].data);
        } catch(e) {
          jsh.log('GOT ERR', e);
          continueCommands = false;
          runState.pop();
        }

        return;
      }
      // console.log(i, command[i]);
    }
  }
  var isjs = true;
  var ret;
  if(commands.length === 1 && isFunction(commands[0])) {
    try {
      ret = runJavaScript(commands[0]);
    } catch(e) {
      if(isJSError(e)) {
        console.log('e3 ' + e);
        isjs = false;
      } else {
        throw e;
      }
    }
  } else {
    try {
      line = Tokenizer.stripEscapes(replaceVariables(line, commands));
      jsh.log("trying the entire thing: '" + line + "'");
      ret = eval.call(global, line);
    } catch(e) {
      if(isJSError(e)) {
        jsh.log('e4 ' + e);
        isjs = false;
      } else {
        throw e;
      }
    }
  }
  if(isjs) {
    jsh.log('is js, ret ' + JSON.stringify(ret));
    if(hasWait(ret)) {
      jsh.log('has wait foo');
      return;
    }
    var silent = false;
    var output;
    if(ret instanceof Object && ret.jsh instanceof Object) {
      if(ret.jsh.silentReturnValue) {
        silent = true;
      } else {
        output = ret.jsh.ret;
      }
    } else {
      output = ret;
    }
    if(!silent && (output !== undefined || jsh.config.printUndefinedReturn)) {
      if(output instanceof Object) {
        try {
          output = jsh.config.prettyReturnValues
            ? JSON.stringify(output, null, jsh.config.prettyReturnValues)
            : JSON.stringify(output);
        } catch(err) {}
      }
      jsh.jshNative.stdout(output, '\n');
    }
    runState.update(jsReturn(ret));
    runState.pop();
    return;
  }

  try {
    runTokens(commands, 0);
  } catch(e) {
    console.log('e5 ' + e);
    runState.pop();
  }
}

function setupEnv() {
  for(var i in process.env) {
    if(i !== undefined) global[i] = process.env[i];
  }
}

function setupBuiltins() {
  var builtins = require('Builtins');
  for(var i in builtins) {
    global[i] = builtins[i];
  }
}

jsh.environment = function() {
  var env = [];
  for(var i in global) {
    if(typeof global[i] === 'string' || typeof global[i] === 'number') {
      env.push(i + '=' + global[i]);
    }
  }
  return env;
};

function loadRCFile(file) {
  var contents;
  try {
    contents = fs.readFileSync(file, { encoding: 'utf8' });
  } catch(err) {
    return false;
  }

  try {
    eval(contents);
  } catch(err) {
    console.error('Exception in ' + file, err);
    return false;
  }
  return true;
}

setupEnv();
setupBuiltins();
runState = new RunState();

loadRCFile('/etc/jshrc.js');
loadRCFile(process.env.HOME + '/.jsh/jshrc.js');

// first callback function handles input, the second handles completion
read = new rl.ReadLine(
  jsh.prompt(),
  function(data) {
    // handle input
    if(data === undefined) {
      read.cleanup();
      Job.cleanup();
      jsh.jshNative.cleanup();
      process.exit();
    }

    try {
      runState.push(function() {
        read.resume(jsh.prompt());
      });
      runLine(data, runState);
    } catch(e) {
      console.log('e6 ' + e);
      read.resume(jsh.prompt());
    }
  },
  function(data) {
    return jsh.completion.complete(data);
  }
);
