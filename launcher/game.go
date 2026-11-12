package main

// Half-Life's side: Steam's libraries (where Half-Life 2: Deathmatch is), the HalfCraft server's client package (its
// game folder and Minecraft mods, as manifest.json lists them), and the engine on that game folder, connected to the
// server.

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
)

// SteamRoot is where Steam is installed ("" if it isn't found).
func SteamRoot() string {
	var candidates []string
	if runtime.GOOS == "windows" {
		candidates = []string{
			filepath.Join(os.Getenv("ProgramFiles(x86)"), "Steam"),
			filepath.Join(os.Getenv("ProgramFiles"), "Steam"),
			`C:\Steam`,
		}
	} else {
		home, _ := os.UserHomeDir()
		candidates = []string{filepath.Join(home, ".local/share/Steam"), filepath.Join(home, ".steam/steam"),
			filepath.Join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam")}
	}
	for _, c := range candidates {
		if st, err := os.Stat(filepath.Join(c, "steamapps")); err == nil && st.IsDir() {
			return c
		}
	}
	return ""
}

// SteamLibraries are Steam's library folders (its libraryfolders.vdf's paths).
func SteamLibraries(root string) []string {
	libraries := []string{root}
	data, err := os.ReadFile(filepath.Join(root, "steamapps", "libraryfolders.vdf"))
	if err != nil {
		return libraries
	}
	for _, m := range regexp.MustCompile(`"path"\s+"([^"]+)"`).FindAllStringSubmatch(string(data), -1) {
		path := strings.ReplaceAll(m[1], `\\`, `\`)
		if !strings.EqualFold(filepath.Clean(path), filepath.Clean(root)) {
			libraries = append(libraries, path)
		}
	}
	return libraries
}

// FindInLibraries is the first library's steamapps/common/<relative> that exists.
func FindInLibraries(root, relative string) string {
	for _, library := range SteamLibraries(root) {
		path := filepath.Join(library, "steamapps", "common", relative)
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}
	return ""
}

// DetectDeathmatch is Half-Life 2: Deathmatch's install folder, "" if Steam has none.
func DetectDeathmatch() string {
	root := SteamRoot()
	if root == "" {
		return ""
	}
	return FindInLibraries(root, "Half-Life 2 Deathmatch")
}

// engineExe is the engine's executable in the deathmatch folder.
func engineExe(deathmatch string) string {
	if runtime.GOOS == "windows" {
		return filepath.Join(deathmatch, "hl2mp_win64.exe")
	}
	return filepath.Join(deathmatch, "hl2mp_linux64")
}

// CheckDeathmatch says what's wrong with a deathmatch folder, nil if the engine is there.
func CheckDeathmatch(folder string) error {
	if folder == "" {
		return errors.New("Half-Life 2: Deathmatch isn't found: install it in Steam (it's free with Half-Life 2), or name its folder")
	}
	if _, err := os.Stat(engineExe(folder)); err != nil {
		return fmt.Errorf("no %s in %s", filepath.Base(engineExe(folder)), folder)
	}
	return nil
}

// ManifestFile is one file of the server's client package.
type ManifestFile struct {
	Path   string `json:"path"`   // where it goes, under the launcher's folder ("game-hl2dm/...", "minecraft/mods/...")
	URL    string `json:"url"`    // where it comes from: an absolute url, or "" for the package's own path
	Sha256 string `json:"sha256"` // or
	Sha512 string `json:"sha512"`
	Size   int64  `json:"size"`
}

// Manifest is the server's client package (tools/server/network.py package).
type Manifest struct {
	Version   string                    `json:"version"`
	Minecraft string                    `json:"minecraft"`
	Loader    string                    `json:"fabric_loader"`
	SrcdsPort int                       `json:"srcds_port"`
	Files     map[string][]ManifestFile `json:"files"` // by platform ("linux", "windows", "any")
}

func platform() string {
	if runtime.GOOS == "windows" {
		return "windows"
	}
	return "linux"
}

// FetchManifest reads the server's client package description.
func FetchManifest(server string) (*Manifest, error) {
	var m Manifest
	if err := getJSON(packageURL(server)+"manifest.json", &m); err != nil {
		return nil, fmt.Errorf("the server's client package: %w", err)
	}
	if len(m.Files[platform()]) == 0 {
		return nil, fmt.Errorf("the server has no HalfCraft build for %s yet", platform())
	}
	return &m, nil
}

// packageURL is where the server serves its client package (network.py serve: port 27080).
func packageURL(server string) string {
	host := server
	if i := strings.LastIndex(host, ":"); i > 0 {
		host = host[:i]
	}
	return "http://" + host + ":27080/"
}

// SyncPackage brings the launcher's copy of the client package up to date.
func SyncPackage(server, root string, m *Manifest, log func(string)) error {
	files := append(append([]ManifestFile{}, m.Files["any"]...), m.Files[platform()]...)
	var jobs []func() error
	for _, f := range files {
		path := filepath.Join(root, filepath.FromSlash(f.Path))
		if strings.Contains(f.Path, "..") {
			return fmt.Errorf("a file outside the launcher's folder: %s", f.Path)
		}
		url := f.URL
		if url == "" {
			url = packageURL(server) + f.Path
		}
		kind, want := "sha256", f.Sha256
		if f.Sha512 != "" {
			kind, want = "sha512", f.Sha512
		}
		jobs = append(jobs, func() error {
			if err := fetch(url, path, kind, want); err != nil {
				return err
			}
			if strings.HasSuffix(path, ".so") || strings.HasSuffix(path, "_linux64") {
				return os.Chmod(path, 0o755)
			}
			return nil
		})
	}
	log(fmt.Sprintf("HalfCraft %s: %d files", m.Version, len(jobs)))
	return parallel(jobs)
}

// EngineCommand is the engine on the game folder, connecting to the server's hub.
func EngineCommand(deathmatch, gameFolder, server string, srcdsPort int, extra []string) (*exec.Cmd, error) {
	host := server
	if i := strings.LastIndex(host, ":"); i > 0 {
		host = host[:i]
	}
	args := []string{"-game", gameFolder, "-novid", "-insecure", "+connect", fmt.Sprintf("%s:%d", host, srcdsPort)}
	args = append(args, extra...)
	env := append(os.Environ(), "SteamAppId=320", "SteamGameId=320")
	if runtime.GOOS == "windows" {
		cmd := exec.Command(engineExe(deathmatch), args...)
		cmd.Dir = deathmatch
		cmd.Env = env
		return cmd, nil
	}
	// linux: in the steam runtime's sniper container, through its hl2mp.sh (it refuses outside it)
	sniper := FindInLibraries(SteamRoot(), filepath.Join("SteamLinuxRuntime_sniper", "run"))
	if sniper == "" {
		return nil, errors.New("Steam Linux Runtime 3.0 (sniper) isn't installed: Steam installs it with Half-Life 2: Deathmatch")
	}
	cmd := exec.Command(sniper, append([]string{"--", filepath.Join(deathmatch, "hl2mp.sh")}, args...)...)
	cmd.Dir = deathmatch
	cmd.Env = append(env, "PRESSURE_VESSEL_FILESYSTEMS_RW="+filepath.Dir(gameFolder))
	return cmd, nil
}
