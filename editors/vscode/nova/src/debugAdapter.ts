// Nova's debug adapter: VS Code's Debug Adapter Protocol on one side, Nova's step debugger (nova --debug) on the
// other. The debugger's protocol (compiler/include/debugger.h) is lines: commands on its stdin, and events on its
// stdout, each "\x1e" + JSON + "\n", among the program's own output. At each stop the debugger sends every frame
// with its variables' values, so the adapter answers the stack, scopes and variables from that, with no more
// round trips.
//
// The extension runs it in its own process (DebugAdapterInlineImplementation); `node dist/debugAdapter.js` runs it
// alone on stdio, as the tests do.
import { ChildProcess, spawn } from 'child_process';
import * as path from 'path';
import {
  DebugSession, Event, ExitedEvent, Handles, InitializedEvent, OutputEvent, Scope, Source, StackFrame, StoppedEvent,
  TerminatedEvent, Thread,
} from '@vscode/debugadapter';
import { DebugProtocol } from '@vscode/debugprotocol';

export interface LaunchArguments extends DebugProtocol.LaunchRequestArguments {
  program: string;       // the .nova file to debug
  stopOnEntry?: boolean; // stop at the program's first statement
  novaPath?: string;     // the compiler (nova); the extension fills it in
  novaHome?: string;     // NOVA_HOME, where 'orbit' finds the standard library
  cwd?: string;          // default: the program's folder
}

interface Variable {
  name: string;
  type: string;
  value: string;
  children?: Variable[];
}

interface Frame {
  name: string;
  file: string;
  line: number;
  column: number;
  variables: Variable[];
}

interface Stop {
  reason: string;
  text: string;
  frames: Frame[];
}

const THREAD = 1;                 // a Nova program has one thread
const EVENT = '\x1e';             // where an event starts in the debugger's output
export const INPUT_EVENT = 'nova.input';    // custom event: the program called input()
export const INPUT_REQUEST = 'nova.input';  // custom request: its value, { value: string }

export class NovaDebugSession extends DebugSession {
  private nova?: ChildProcess;
  private buffer = '';
  private paused?: Stop;                       // the state at the last stop; none while the program runs
  private variables = new Handles<Variable[]>();
  private ready = false;                     // the debugger has compiled the program and waits for commands
  private configured = false;                // VS Code has sent the breakpoints (configurationDone)
  private started = false;
  private stopOnEntry = false;

  public constructor() {
    super();
    this.setDebuggerLinesStartAt1(true);
    this.setDebuggerColumnsStartAt1(true);
  }

  protected initializeRequest(response: DebugProtocol.InitializeResponse): void {
    response.body = {
      supportsConfigurationDoneRequest: true,
      supportsEvaluateForHovers: true,
      supportsTerminateRequest: true,
    };
    this.sendResponse(response);
  }

  protected launchRequest(response: DebugProtocol.LaunchResponse, args: LaunchArguments): void {
    this.stopOnEntry = !!args.stopOnEntry;
    const env = { ...process.env, ...(args.novaHome ? { NOVA_HOME: args.novaHome } : {}) };
    const nova = spawn(args.novaPath || 'nova', ['--debug', args.program], {
      cwd: args.cwd || path.dirname(args.program),
      env,
    });
    this.nova = nova;
    nova.stdout!.setEncoding('utf8');
    nova.stderr!.setEncoding('utf8');
    nova.stdout!.on('data', (chunk: string) => this.readOutput(chunk));
    nova.stderr!.on('data', (chunk: string) => this.sendEvent(new OutputEvent(chunk, 'stderr')));
    nova.on('error', e => {
      this.sendEvent(new OutputEvent(`Nova could not start: ${e.message}\n`, 'stderr'));
      this.sendEvent(new TerminatedEvent());
    });
    nova.on('exit', code => {
      if (this.buffer) this.sendEvent(new OutputEvent(this.buffer, 'stdout'));
      this.buffer = '';
      this.sendEvent(new ExitedEvent(code ?? 0));
      this.sendEvent(new TerminatedEvent());
    });
    this.sendResponse(response);
  }

  // The debugger's output: the program's own text, and events, each starting at \x1e and ending its line
  private readOutput(chunk: string): void {
    this.buffer += chunk;
    for (;;) {
      const at = this.buffer.indexOf(EVENT);
      if (at < 0) {
        if (this.buffer) this.sendEvent(new OutputEvent(this.buffer, 'stdout'));
        this.buffer = '';
        return;
      }
      if (at > 0) this.sendEvent(new OutputEvent(this.buffer.slice(0, at), 'stdout'));
      const end = this.buffer.indexOf('\n', at);
      if (end < 0) {
        this.buffer = this.buffer.slice(at);  // the rest of the event is still to come
        return;
      }
      const json = this.buffer.slice(at + 1, end);
      this.buffer = this.buffer.slice(end + 1);
      try {
        this.onEngineEvent(JSON.parse(json));
      } catch (e) {
        this.sendEvent(new OutputEvent(`Nova's debugger sent what the adapter could not read: ${json}\n`, 'stderr'));
      }
    }
  }

  private onEngineEvent(e: { event: string } & Partial<Stop>): void {
    if (e.event === 'ready') {
      this.ready = true;
      this.sendEvent(new InitializedEvent());  // VS Code now sends the breakpoints, then configurationDone
      this.runProgram();
    } else if (e.event === 'stopped') {
      this.paused = { reason: e.reason ?? 'step', text: e.text ?? '', frames: e.frames ?? [] };
      this.variables.reset();
      this.sendEvent(new StoppedEvent(this.paused.reason, THREAD, this.paused.text || undefined));
    } else if (e.event === 'input') {
      this.sendEvent(new Event(INPUT_EVENT, {}));
    }
  }

  // The program runs once the debugger is ready and VS Code has set the breakpoints
  private runProgram(): void {
    if (!this.ready || !this.configured || this.started) return;
    this.started = true;
    this.tell(this.stopOnEntry ? 'stepIn' : 'continue');
  }

  private tell(line: string): void {
    this.nova?.stdin?.write(line + '\n');
  }

  protected configurationDoneRequest(response: DebugProtocol.ConfigurationDoneResponse): void {
    this.configured = true;
    this.runProgram();
    this.sendResponse(response);
  }

  // A file's breakpoints, all at once. The debugger reads them as they come, while the program runs too, so they
  // apply at once.
  protected setBreakPointsRequest(response: DebugProtocol.SetBreakpointsResponse,
                                  args: DebugProtocol.SetBreakpointsArguments): void {
    const lines = (args.breakpoints ?? []).map(b => this.convertClientLineToDebugger(b.line));
    if (args.source.path) this.tell(`breakpoints ${args.source.path}\t${lines.join(',')}`);
    response.body = {
      breakpoints: lines.map(line => ({ verified: true, line: this.convertDebuggerLineToClient(line) })),
    };
    this.sendResponse(response);
  }

  protected threadsRequest(response: DebugProtocol.ThreadsResponse): void {
    response.body = { threads: [new Thread(THREAD, 'main')] };
    this.sendResponse(response);
  }

  protected stackTraceRequest(response: DebugProtocol.StackTraceResponse): void {
    const frames = this.paused?.frames ?? [];
    response.body = {
      stackFrames: frames.map((f, i) => new StackFrame(i, f.name,
        f.file ? new Source(path.basename(f.file), this.convertDebuggerPathToClient(f.file)) : undefined,
        this.convertDebuggerLineToClient(f.line), this.convertDebuggerColumnToClient(f.column))),
      totalFrames: frames.length,
    };
    this.sendResponse(response);
  }

  protected scopesRequest(response: DebugProtocol.ScopesResponse, args: DebugProtocol.ScopesArguments): void {
    const frame = this.paused?.frames[args.frameId];
    response.body = { scopes: frame ? [new Scope('Locals', this.variables.create(frame.variables), false)] : [] };
    this.sendResponse(response);
  }

  protected variablesRequest(response: DebugProtocol.VariablesResponse,
                             args: DebugProtocol.VariablesArguments): void {
    response.body = { variables: (this.variables.get(args.variablesReference) ?? []).map(v => this.toDap(v)) };
    this.sendResponse(response);
  }

  private toDap(v: Variable): DebugProtocol.Variable {
    return {
      name: v.name,
      value: v.value,
      type: v.type,
      variablesReference: v.children && v.children.length ? this.variables.create(v.children) : 0,
    };
  }

  // A hover or a watch: a variable of the frame, or a path into one (p.x, v[0], t[1])
  protected evaluateRequest(response: DebugProtocol.EvaluateResponse, args: DebugProtocol.EvaluateArguments): void {
    const frame = this.paused?.frames[args.frameId ?? 0];
    const parts = args.expression.trim().match(/[A-Za-z_][A-Za-z0-9_]*|\[[^\]]*\]/g) ?? [];
    let scope = frame?.variables ?? [];
    let found: Variable | undefined;
    for (const part of parts) {
      found = scope.find(v => v.name === part || v.name === `[${part.replace(/^\[|\]$/g, '')}]`);
      if (!found) break;
      scope = found.children ?? [];
    }
    if (!found || !parts.length) {
      this.sendErrorResponse(response, 1, `'${args.expression}' is not a variable here`);
      return;
    }
    const value = this.toDap(found);
    response.body = { result: value.value, type: value.type, variablesReference: value.variablesReference };
    this.sendResponse(response);
  }

  protected continueRequest(response: DebugProtocol.ContinueResponse): void {
    this.resume('continue');
    response.body = { allThreadsContinued: true };
    this.sendResponse(response);
  }

  protected nextRequest(response: DebugProtocol.NextResponse): void {
    this.resume('next');
    this.sendResponse(response);
  }

  protected stepInRequest(response: DebugProtocol.StepInResponse): void {
    this.resume('stepIn');
    this.sendResponse(response);
  }

  protected stepOutRequest(response: DebugProtocol.StepOutResponse): void {
    this.resume('stepOut');
    this.sendResponse(response);
  }

  private resume(command: string): void {
    this.paused = undefined;
    this.tell(command);
  }

  // The debugger stops the running program at its next instruction, and says so with a stop (reason "pause")
  protected pauseRequest(response: DebugProtocol.PauseResponse): void {
    this.tell('pause');
    this.sendResponse(response);
  }

  // input(): the extension asks for the value and sends it here
  protected customRequest(command: string, response: DebugProtocol.Response, args: { value?: string }): void {
    if (command !== INPUT_REQUEST) {
      super.customRequest(command, response, args);
      return;
    }
    this.tell(String(args?.value ?? '').replace(/[\r\n]+/g, ' '));
    this.sendResponse(response);
  }

  protected terminateRequest(response: DebugProtocol.TerminateResponse): void {
    this.endProgram();
    this.sendResponse(response);
  }

  protected disconnectRequest(response: DebugProtocol.DisconnectResponse): void {
    this.endProgram();
    this.sendResponse(response);
  }

  // stop ends the program where it is, stopped or running; one that does not end soon (stuck in a long tensor
  // operation, say) is killed
  private endProgram(): void {
    const nova = this.nova;
    if (!nova || nova.exitCode !== null) return;
    this.tell('stop');
    setTimeout(() => { if (nova.exitCode === null) nova.kill(); }, 500);
  }
}

// Run alone (node dist/debugAdapter.js): the protocol on stdio
if (require.main === module) DebugSession.run(NovaDebugSession);
