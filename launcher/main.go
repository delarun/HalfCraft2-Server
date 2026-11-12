// HalfCraft's launcher (docs/server/PLAN.md, S4): one program for Linux and Windows that signs the player in to
// Minecraft with their Microsoft account, installs Minecraft with Fabric and the HalfCraft server's client package, and
// starts both games connected to the server. Its window is a page in the browser, served on localhost.
package main

import (
	_ "embed"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	webview "github.com/webview/webview_go"
)

func init() {
	// the window's toolkit (gtk, win32) runs on the main thread
	runtime.LockOSThread()
}

// Version is the launcher's own.
const Version = "0.1.0"

//go:embed ui.html
var page []byte

// Config is what the launcher keeps between runs (config.json in its folder).
type Config struct {
	Server     string   `json:"server"`
	Deathmatch string   `json:"deathmatch"`
	Account    *Account `json:"account,omitempty"`
}

// Launcher is its state: the config, what it's doing, and its log.
type Launcher struct {
	mu      sync.Mutex
	root    string
	config  Config
	busy    string
	code    *DeviceCode
	lines   []string
	running bool
}

func dataRoot() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(os.Getenv("APPDATA"), "HalfCraft")
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, "halfcraft")
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "halfcraft")
}

func (l *Launcher) logf(format string, args ...any) {
	line := time.Now().Format("15:04:05 ") + fmt.Sprintf(format, args...)
	log.Print(line)
	l.mu.Lock()
	l.lines = append(l.lines, line)
	if len(l.lines) > 400 {
		l.lines = l.lines[len(l.lines)-400:]
	}
	l.mu.Unlock()
}

func (l *Launcher) load() {
	data, err := os.ReadFile(filepath.Join(l.root, "config.json"))
	if err == nil {
		json.Unmarshal(data, &l.config)
	}
	if l.config.Deathmatch == "" {
		l.config.Deathmatch = DetectDeathmatch()
	}
}

func (l *Launcher) save() {
	os.MkdirAll(l.root, 0o755)
	data, _ := json.MarshalIndent(l.config, "", "  ")
	// the refresh token is in it: the player's own
	os.WriteFile(filepath.Join(l.root, "config.json"), data, 0o600)
}

type state struct {
	Server     string      `json:"server"`
	Deathmatch string      `json:"deathmatch"`
	Problem    string      `json:"problem"`
	Account    string      `json:"account"`
	Busy       string      `json:"busy"`
	Code       *DeviceCode `json:"code"`
	Running    bool        `json:"running"`
	Log        []string    `json:"log"`
	Version    string      `json:"version"`
}

func (l *Launcher) state() state {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := state{Server: l.config.Server, Deathmatch: l.config.Deathmatch, Busy: l.busy, Code: l.code, Running: l.running,
		Log: append([]string{}, l.lines...), Version: Version}
	if err := CheckDeathmatch(l.config.Deathmatch); err != nil {
		s.Problem = err.Error()
	}
	if l.config.Account != nil {
		s.Account = l.config.Account.Name
	}
	return s
}

// task runs one thing at a time in the background, under a name the page shows.
func (l *Launcher) task(name string, work func() error) bool {
	l.mu.Lock()
	if l.busy != "" {
		l.mu.Unlock()
		return false
	}
	l.busy = name
	l.mu.Unlock()
	go func() {
		err := work()
		l.mu.Lock()
		l.busy = ""
		l.code = nil
		l.mu.Unlock()
		if err != nil {
			l.logf("%s: %v", name, err)
		}
	}()
	return true
}

func (l *Launcher) signIn() error {
	code, err := StartDeviceCode()
	if err != nil {
		return err
	}
	l.mu.Lock()
	l.code = code
	l.mu.Unlock()
	l.logf("sign in at %s with the code %s", code.VerificationURI, code.UserCode)
	openBrowser(code.VerificationURI)
	account, err := WaitDeviceCode(code)
	if err != nil {
		return err
	}
	l.mu.Lock()
	l.config.Account = account
	l.mu.Unlock()
	l.save()
	l.logf("signed in as %s", account.Name)
	return nil
}

func (l *Launcher) play() error {
	l.mu.Lock()
	server, deathmatch, account := l.config.Server, l.config.Deathmatch, l.config.Account
	l.mu.Unlock()
	if server == "" {
		return fmt.Errorf("name the server")
	}
	if err := CheckDeathmatch(deathmatch); err != nil {
		return err
	}
	if account == nil {
		return fmt.Errorf("sign in to Minecraft first")
	}
	l.logf("signing in as %s", account.Name)
	fresh, err := Refresh(account)
	if err != nil {
		return fmt.Errorf("sign in again: %w", err)
	}
	l.mu.Lock()
	l.config.Account = fresh
	l.mu.Unlock()
	l.save()

	manifest, prepared, err := l.install()
	if err != nil {
		return err
	}

	gameDir := filepath.Join(l.root, "minecraft", "instance")
	os.MkdirAll(filepath.Join(gameDir, "mods"), 0o755)
	mc := prepared.Command(fresh, gameDir, filepath.Join(l.root, "minecraft", "assets"),
		[]string{"-Xms512M", "-Xmx4G", "--enable-native-access=ALL-UNNAMED", "-Dhalfcraft.startHidden=true"})
	mcLog, _ := os.Create(filepath.Join(l.root, "minecraft.log"))
	mc.Stdout, mc.Stderr = mcLog, mcLog
	l.logf("starting Minecraft")
	if err := mc.Start(); err != nil {
		return err
	}
	engine, err := EngineCommand(deathmatch, filepath.Join(l.root, "game-hl2dm"), server, manifest.SrcdsPort, nil)
	if err != nil {
		mc.Process.Kill()
		return err
	}
	engineLog, _ := os.Create(filepath.Join(l.root, "engine.log"))
	engine.Stdout, engine.Stderr = engineLog, engineLog
	l.logf("starting Half-Life 2: Deathmatch")
	if err := engine.Start(); err != nil {
		mc.Process.Kill()
		return err
	}
	l.mu.Lock()
	l.running = true
	l.mu.Unlock()
	go func() {
		engine.Wait()
		l.logf("Half-Life closed")
		// its minecraft goes with it (the mod quits when the host does, unless it's asked not to)
		mc.Wait()
		l.logf("Minecraft closed")
		l.mu.Lock()
		l.running = false
		l.mu.Unlock()
	}()
	return nil
}

// install brings the server's client package and Minecraft with Fabric up to date.
func (l *Launcher) install() (*Manifest, *Prepared, error) {
	l.mu.Lock()
	server := l.config.Server
	l.mu.Unlock()
	if server == "" {
		return nil, nil, fmt.Errorf("name the server")
	}
	manifest, err := FetchManifest(server)
	if err != nil {
		return nil, nil, err
	}
	if err := SyncPackage(server, l.root, manifest, func(s string) { l.logf("%s", s) }); err != nil {
		return nil, nil, err
	}
	installer := &Installer{Root: filepath.Join(l.root, "minecraft"), Log: func(s string) { l.logf("%s", s) }}
	prepared, err := installer.Install(manifest.Minecraft, manifest.Loader)
	return manifest, prepared, err
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		cmd = exec.Command("open", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	cmd.Start()
}

func main() {
	server := flag.String("server", "", "the HalfCraft server (host or host:port), saved for next time")
	browser := flag.Bool("browser", false, "show the launcher in the browser instead of a window of its own")
	testMinecraft := flag.Bool("test-minecraft", false, "a developer's: install, then start Minecraft alone with an offline player")
	prepare := flag.Bool("prepare", false, "install what playing needs (the server's package, Minecraft, Fabric, Java), then exit")
	flag.Parse()

	l := &Launcher{root: dataRoot()}
	l.load()
	if *server != "" {
		l.config.Server = *server
		l.save()
	}
	if *prepare || *testMinecraft {
		_, prepared, err := l.install()
		if err != nil {
			log.Fatal(err)
		}
		log.Print("ready")
		if *testMinecraft {
			// a smoke test of the command line: an offline player, on a link of its own (no half-life answers it)
			gameDir := filepath.Join(l.root, "minecraft", "instance")
			cmd := prepared.Command(&Account{Name: "LauncherTest", UUID: "00000000000000000000000000000000", AccessToken: "0"}, gameDir,
				filepath.Join(l.root, "minecraft", "assets"), []string{"-Xmx2G", "--enable-native-access=ALL-UNNAMED", "-Dhalfcraft.startHidden=true"})
			cmd.Env = append(os.Environ(), "HALFCRAFT_LINK=launchertest")
			cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
			log.Fatal(cmd.Run())
		}
		return
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(page)
	})
	mux.HandleFunc("/api/state", func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(l.state())
	})
	mux.HandleFunc("/api/config", func(w http.ResponseWriter, r *http.Request) {
		var in struct{ Server, Deathmatch string }
		if json.NewDecoder(r.Body).Decode(&in) == nil {
			l.mu.Lock()
			l.config.Server = strings.TrimSpace(in.Server)
			l.config.Deathmatch = strings.TrimSpace(in.Deathmatch)
			l.mu.Unlock()
			l.save()
		}
		json.NewEncoder(w).Encode(l.state())
	})
	mux.HandleFunc("/api/signin", func(w http.ResponseWriter, r *http.Request) {
		l.task("sign-in", l.signIn)
		json.NewEncoder(w).Encode(l.state())
	})
	mux.HandleFunc("/api/signout", func(w http.ResponseWriter, r *http.Request) {
		l.mu.Lock()
		l.config.Account = nil
		l.mu.Unlock()
		l.save()
		json.NewEncoder(w).Encode(l.state())
	})
	mux.HandleFunc("/api/play", func(w http.ResponseWriter, r *http.Request) {
		l.task("play", l.play)
		json.NewEncoder(w).Encode(l.state())
	})

	// localhost only: the page can start programs
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}
	address := "http://" + listener.Addr().String() + "/"
	log.Printf("HalfCraft launcher %s: %s (folder %s)", Version, address, l.root)
	srv := &http.Server{Handler: guard(mux), ReadHeaderTimeout: 10 * time.Second}
	if *browser {
		openBrowser(address)
		log.Fatal(srv.Serve(listener))
	}
	go func() { log.Fatal(srv.Serve(listener)) }()
	// a window of its own showing the page (webkit2gtk on linux, webview2 on windows); closing it ends the launcher,
	// and the games it started go on
	window := webview.New(false)
	if window == nil {
		log.Print("no window (webkit2gtk or webview2 missing): the browser instead")
		openBrowser(address)
		select {}
	}
	defer window.Destroy()
	window.SetTitle("HalfCraft")
	window.SetSize(820, 760, webview.HintNone)
	window.Navigate(address)
	window.Run()
}

// guard keeps the page's api to this machine's own page: the Host must be localhost's (no other site's name resolved to
// it), and what changes anything is a POST from no other origin.
func guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.Host, "127.0.0.1:") {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if strings.HasPrefix(r.URL.Path, "/api/") && r.URL.Path != "/api/state" {
			origin := r.Header.Get("Origin")
			if r.Method != http.MethodPost || (origin != "" && origin != "http://"+r.Host) {
				http.Error(w, "forbidden", http.StatusForbidden)
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}
