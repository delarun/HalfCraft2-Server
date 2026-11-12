package main

// Minecraft as the official launcher installs it: the version's json from Mojang's manifest, its libraries, client jar
// and assets, the Java runtime Mojang names for it, and Fabric's loader over it (its profile json from Fabric's meta).
// Everything is checked against the hashes the jsons give and downloaded only once, under the launcher's folder.

import (
	"crypto/sha1"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

const (
	versionManifest = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
	javaManifest    = "https://launchermeta.mojang.com/v1/products/java-runtime/2ec0cc96c44e5a76b9c8b7c39df7210883d12871/all.json"
	assetHost       = "https://resources.download.minecraft.net/"
)

type rule struct {
	Action   string            `json:"action"`
	OS       map[string]string `json:"os"`
	Features map[string]bool   `json:"features"`
}

type artifact struct {
	Path string `json:"path"`
	URL  string `json:"url"`
	Sha1 string `json:"sha1"`
	Size int64  `json:"size"`
}

type library struct {
	Name      string `json:"name"`
	URL       string `json:"url"` // fabric's: a maven repository
	Sha1      string `json:"sha1"`
	Downloads struct {
		Artifact *artifact `json:"artifact"`
	} `json:"downloads"`
	Rules []rule `json:"rules"`
}

type argument struct {
	Rules []rule
	Value []string
}

func (a *argument) UnmarshalJSON(data []byte) error {
	var plain string
	if json.Unmarshal(data, &plain) == nil {
		a.Value = []string{plain}
		return nil
	}
	var ruled struct {
		Rules []rule          `json:"rules"`
		Value json.RawMessage `json:"value"`
	}
	if err := json.Unmarshal(data, &ruled); err != nil {
		return err
	}
	a.Rules = ruled.Rules
	if json.Unmarshal(ruled.Value, &plain) == nil {
		a.Value = []string{plain}
		return nil
	}
	return json.Unmarshal(ruled.Value, &a.Value)
}

type versionJSON struct {
	ID           string    `json:"id"`
	InheritsFrom string    `json:"inheritsFrom"`
	MainClass    string    `json:"mainClass"`
	Type         string    `json:"type"`
	Libraries    []library `json:"libraries"`
	Arguments    struct {
		Game []argument `json:"game"`
		JVM  []argument `json:"jvm"`
	} `json:"arguments"`
	AssetIndex struct {
		ID   string `json:"id"`
		URL  string `json:"url"`
		Sha1 string `json:"sha1"`
	} `json:"assetIndex"`
	Downloads struct {
		Client artifact `json:"client"`
	} `json:"downloads"`
	JavaVersion struct {
		Component    string `json:"component"`
		MajorVersion int    `json:"majorVersion"`
	} `json:"javaVersion"`
}

// mojangOS is the name Mojang's rules give this system.
func mojangOS() string {
	switch runtime.GOOS {
	case "windows":
		return "windows"
	case "darwin":
		return "osx"
	}
	return "linux"
}

func allowed(rules []rule) bool {
	if len(rules) == 0 {
		return true
	}
	ok := false
	for _, r := range rules {
		if len(r.Features) > 0 {
			continue // demo, custom resolution, quick play: none of them
		}
		if name := r.OS["name"]; name != "" && name != mojangOS() {
			continue
		}
		if arch := r.OS["arch"]; arch != "" && arch != "x86" {
			// "arch": "x86" is a 32-bit rule
		} else if arch == "x86" {
			continue
		}
		ok = r.Action == "allow"
	}
	return ok
}

// Installer downloads into the launcher's minecraft folder, reporting what it does.
type Installer struct {
	Root string
	Log  func(string)
}

func getJSON(url string, out any) error {
	resp, err := httpClient.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%s: HTTP %d", url, resp.StatusCode)
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

func fileHash(path string, h hash.Hash) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	if _, err := io.Copy(h, f); err != nil {
		return ""
	}
	return hex.EncodeToString(h.Sum(nil))
}

func hasherFor(kind string) hash.Hash {
	switch kind {
	case "sha256":
		return sha256.New()
	case "sha512":
		return sha512.New()
	}
	return sha1.New()
}

// fetch downloads url to path unless path already has that hash (kind sha1, sha256 or sha512; "" checks nothing but
// that it's there).
func fetch(url, path, kind, want string) error {
	if want != "" && strings.EqualFold(fileHash(path, hasherFor(kind)), want) {
		return nil
	}
	if want == "" {
		if _, err := os.Stat(path); err == nil {
			return nil
		}
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	resp, err := httpClient.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%s: HTTP %d", url, resp.StatusCode)
	}
	temporary := path + ".part"
	f, err := os.Create(temporary)
	if err != nil {
		return err
	}
	h := hasherFor(kind)
	_, err = io.Copy(io.MultiWriter(f, h), resp.Body)
	f.Close()
	if err != nil {
		os.Remove(temporary)
		return err
	}
	if want != "" && !strings.EqualFold(hex.EncodeToString(h.Sum(nil)), want) {
		os.Remove(temporary)
		return fmt.Errorf("%s: the download doesn't match its %s", url, kind)
	}
	return os.Rename(temporary, path)
}

// parallel runs jobs, eight at a time, and returns the first error.
func parallel(jobs []func() error) error {
	var wg sync.WaitGroup
	var once sync.Once
	var first error
	slots := make(chan struct{}, 8)
	for _, job := range jobs {
		wg.Add(1)
		slots <- struct{}{}
		go func(job func() error) {
			defer wg.Done()
			defer func() { <-slots }()
			if err := job(); err != nil {
				once.Do(func() { first = err })
			}
		}(job)
	}
	wg.Wait()
	return first
}

// mavenPath is a maven coordinate's path in a repository (group:artifact:version[:classifier]).
func mavenPath(name string) string {
	parts := strings.Split(name, ":")
	if len(parts) < 3 {
		return name
	}
	group := strings.ReplaceAll(parts[0], ".", "/")
	file := parts[1] + "-" + parts[2]
	if len(parts) > 3 {
		file += "-" + parts[3]
	}
	return group + "/" + parts[1] + "/" + parts[2] + "/" + file + ".jar"
}

// Prepared is an installed Minecraft with Fabric: what launching it needs.
type Prepared struct {
	Version   versionJSON // vanilla's, with fabric's libraries and arguments merged in
	Java      string
	Classpath []string
	Natives   string
}

// Install makes sure Minecraft (mcVersion) with Fabric's loader (loaderVersion) is installed, and returns how to start it.
func (in *Installer) Install(mcVersion, loaderVersion string) (*Prepared, error) {
	in.Log("Minecraft " + mcVersion + ": its version")
	var manifest struct {
		Versions []struct {
			ID   string `json:"id"`
			URL  string `json:"url"`
			Sha1 string `json:"sha1"`
		} `json:"versions"`
	}
	if err := getJSON(versionManifest, &manifest); err != nil {
		return nil, err
	}
	var vanillaURL, vanillaSha string
	for _, v := range manifest.Versions {
		if v.ID == mcVersion {
			vanillaURL, vanillaSha = v.URL, v.Sha1
		}
	}
	if vanillaURL == "" {
		return nil, fmt.Errorf("Mojang has no Minecraft %s", mcVersion)
	}
	versionPath := filepath.Join(in.Root, "versions", mcVersion, mcVersion+".json")
	if err := fetch(vanillaURL, versionPath, "sha1", vanillaSha); err != nil {
		return nil, err
	}
	var vanilla versionJSON
	data, err := os.ReadFile(versionPath)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, &vanilla); err != nil {
		return nil, err
	}

	in.Log("Fabric loader " + loaderVersion)
	var fabric versionJSON
	if err := getJSON(fmt.Sprintf("https://meta.fabricmc.net/v2/versions/loader/%s/%s/profile/json", mcVersion, loaderVersion), &fabric); err != nil {
		return nil, fmt.Errorf("fabric: %w", err)
	}

	libraries := filepath.Join(in.Root, "libraries")
	var jobs []func() error
	var classpath []string
	seen := map[string]bool{}
	key := func(name string) string { // group:artifact[:classifier]: fabric's ASM replaces vanilla's
		p := strings.Split(name, ":")
		if len(p) > 3 {
			return p[0] + ":" + p[1] + ":" + p[3]
		}
		if len(p) > 1 {
			return p[0] + ":" + p[1]
		}
		return name
	}
	for _, lib := range fabric.Libraries {
		path := filepath.Join(libraries, filepath.FromSlash(mavenPath(lib.Name)))
		repo := lib.URL
		if repo == "" {
			repo = "https://maven.fabricmc.net/"
		}
		url, sha := strings.TrimSuffix(repo, "/")+"/"+mavenPath(lib.Name), lib.Sha1
		jobs = append(jobs, func() error { return fetch(url, path, "sha1", sha) })
		classpath = append(classpath, path)
		seen[key(lib.Name)] = true
	}
	for _, lib := range vanilla.Libraries {
		if !allowed(lib.Rules) || lib.Downloads.Artifact == nil || seen[key(lib.Name)] {
			continue
		}
		a := *lib.Downloads.Artifact
		path := filepath.Join(libraries, filepath.FromSlash(a.Path))
		jobs = append(jobs, func() error { return fetch(a.URL, path, "sha1", a.Sha1) })
		classpath = append(classpath, path)
		seen[key(lib.Name)] = true
	}
	client := filepath.Join(in.Root, "versions", mcVersion, mcVersion+".jar")
	jobs = append(jobs, func() error {
		return fetch(vanilla.Downloads.Client.URL, client, "sha1", vanilla.Downloads.Client.Sha1)
	})
	classpath = append(classpath, client)
	in.Log(fmt.Sprintf("libraries: %d files", len(jobs)))
	if err := parallel(jobs); err != nil {
		return nil, err
	}

	if err := in.assets(vanilla); err != nil {
		return nil, err
	}
	java, err := in.java(vanilla.JavaVersion.Component)
	if err != nil {
		return nil, err
	}

	merged := vanilla
	merged.MainClass = fabric.MainClass
	merged.Arguments.Game = append(append([]argument{}, vanilla.Arguments.Game...), fabric.Arguments.Game...)
	merged.Arguments.JVM = append(append([]argument{}, vanilla.Arguments.JVM...), fabric.Arguments.JVM...)
	natives := filepath.Join(in.Root, "natives", mcVersion)
	os.MkdirAll(natives, 0o755)
	return &Prepared{Version: merged, Java: java, Classpath: classpath, Natives: natives}, nil
}

func (in *Installer) assets(v versionJSON) error {
	indexPath := filepath.Join(in.Root, "assets", "indexes", v.AssetIndex.ID+".json")
	if err := fetch(v.AssetIndex.URL, indexPath, "sha1", v.AssetIndex.Sha1); err != nil {
		return err
	}
	var index struct {
		Objects map[string]struct {
			Hash string `json:"hash"`
		} `json:"objects"`
	}
	data, err := os.ReadFile(indexPath)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(data, &index); err != nil {
		return err
	}
	var jobs []func() error
	for _, object := range index.Objects {
		h := object.Hash
		path := filepath.Join(in.Root, "assets", "objects", h[:2], h)
		if _, err := os.Stat(path); err == nil {
			continue // named by its hash: there means right
		}
		jobs = append(jobs, func() error { return fetch(assetHost+h[:2]+"/"+h, path, "sha1", h) })
	}
	if len(jobs) > 0 {
		in.Log(fmt.Sprintf("assets: %d files", len(jobs)))
	}
	return parallel(jobs)
}

// java installs the Java runtime Mojang names for the version (component) and returns its java executable.
func (in *Installer) java(component string) (string, error) {
	platform := "linux"
	if runtime.GOOS == "windows" {
		platform = "windows-x64"
	}
	if component == "" {
		return "", errors.New("the version names no Java runtime")
	}
	var all map[string]map[string][]struct {
		Manifest struct {
			URL string `json:"url"`
		} `json:"manifest"`
	}
	if err := getJSON(javaManifest, &all); err != nil {
		return "", err
	}
	entries := all[platform][component]
	if len(entries) == 0 {
		return "", fmt.Errorf("Mojang has no %s for %s", component, platform)
	}
	var files struct {
		Files map[string]struct {
			Type       string `json:"type"`
			Executable bool   `json:"executable"`
			Target     string `json:"target"`
			Downloads  struct {
				Raw artifact `json:"raw"`
			} `json:"downloads"`
		} `json:"files"`
	}
	if err := getJSON(entries[0].Manifest.URL, &files); err != nil {
		return "", err
	}
	root := filepath.Join(in.Root, "runtime", component)
	var jobs []func() error
	for name, f := range files.Files {
		path := filepath.Join(root, filepath.FromSlash(name))
		switch f.Type {
		case "directory":
			os.MkdirAll(path, 0o755)
		case "link":
			if runtime.GOOS != "windows" {
				os.MkdirAll(filepath.Dir(path), 0o755)
				os.Remove(path)
				os.Symlink(f.Target, path)
			}
		case "file":
			raw, executable := f.Downloads.Raw, f.Executable
			jobs = append(jobs, func() error {
				if err := fetch(raw.URL, path, "sha1", raw.Sha1); err != nil {
					return err
				}
				if executable {
					return os.Chmod(path, 0o755)
				}
				return nil
			})
		}
	}
	in.Log(fmt.Sprintf("Java (%s): %d files", component, len(jobs)))
	if err := parallel(jobs); err != nil {
		return "", err
	}
	if runtime.GOOS == "windows" {
		return filepath.Join(root, "bin", "javaw.exe"), nil
	}
	return filepath.Join(root, "bin", "java"), nil
}

// Command is Minecraft's command line for the account, playing in gameDir.
func (p *Prepared) Command(account *Account, gameDir, assets string, extraJVM []string) *exec.Cmd {
	separator := ":"
	if runtime.GOOS == "windows" {
		separator = ";"
	}
	values := map[string]string{
		"${natives_directory}":   p.Natives,
		"${launcher_name}":       "HalfCraft",
		"${launcher_version}":    Version,
		"${classpath}":           strings.Join(p.Classpath, separator),
		"${classpath_separator}": separator,
		"${library_directory}":   filepath.Join(filepath.Dir(assets), "libraries"),
		"${version_name}":        p.Version.ID,
		"${game_directory}":      gameDir,
		"${assets_root}":         assets,
		"${assets_index_name}":   p.Version.AssetIndex.ID,
		"${auth_player_name}":    account.Name,
		"${auth_uuid}":           account.UUID,
		"${auth_access_token}":   account.AccessToken,
		"${clientid}":            ClientID,
		"${auth_xuid}":           account.Xuid,
		"${user_type}":           "msa",
		"${version_type}":        "HalfCraft",
	}
	expand := func(args []argument) []string {
		var out []string
		for _, a := range args {
			if !allowed(a.Rules) {
				continue
			}
			for _, v := range a.Value {
				for k, value := range values {
					v = strings.ReplaceAll(v, k, value)
				}
				out = append(out, v)
			}
		}
		return out
	}
	args := append([]string{}, extraJVM...)
	args = append(args, expand(p.Version.Arguments.JVM)...)
	args = append(args, p.Version.MainClass)
	args = append(args, expand(p.Version.Arguments.Game)...)
	cmd := exec.Command(p.Java, args...)
	cmd.Dir = gameDir
	return cmd
}
