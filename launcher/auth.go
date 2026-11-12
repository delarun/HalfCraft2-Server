package main

// Microsoft sign-in for Minecraft: the device code flow with the HalfCraft Azure app's client id, then Xbox Live, XSTS
// and Minecraft's services, as the official launcher does. The refresh token is kept (config.go), so the player signs
// in once.

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// ClientID is the HalfCraft Azure app (a public client: no secret).
const ClientID = "41f23721-33c5-4533-923d-f981d3f65040"

const scope = "XboxLive.signin offline_access"

// Account is the signed-in player: Minecraft's profile and the tokens it plays with.
type Account struct {
	Name         string    `json:"name"`
	UUID         string    `json:"uuid"`
	Xuid         string    `json:"xuid"`
	RefreshToken string    `json:"refresh_token"`
	AccessToken  string    `json:"-"` // Minecraft's, for this run only
	Expires      time.Time `json:"-"`
}

// DeviceCode is what the player types at Microsoft's page.
type DeviceCode struct {
	UserCode        string `json:"user_code"`
	VerificationURI string `json:"verification_uri"`
	DeviceCode      string `json:"device_code"`
	Interval        int    `json:"interval"`
	ExpiresIn       int    `json:"expires_in"`
	Message         string `json:"message"`
}

var httpClient = &http.Client{Timeout: 60 * time.Second}

func postForm(endpoint string, form url.Values, out any) error {
	resp, err := httpClient.PostForm(endpoint, form)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode/100 != 2 {
		return &httpError{resp.StatusCode, string(body)}
	}
	return json.Unmarshal(body, out)
}

func postJSON(endpoint string, in, out any, bearer string) error {
	data, _ := json.Marshal(in)
	req, _ := http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(data))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	resp, err := httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode/100 != 2 {
		return &httpError{resp.StatusCode, string(body)}
	}
	return json.Unmarshal(body, out)
}

type httpError struct {
	status int
	body   string
}

func (e *httpError) Error() string {
	return fmt.Sprintf("HTTP %d: %s", e.status, strings.TrimSpace(e.body))
}

// StartDeviceCode asks Microsoft for a code the player enters at its page.
func StartDeviceCode() (*DeviceCode, error) {
	var code DeviceCode
	err := postForm("https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode",
		url.Values{"client_id": {ClientID}, "scope": {scope}}, &code)
	return &code, err
}

type msToken struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	Error        string `json:"error"`
}

// WaitDeviceCode polls until the player signed in at Microsoft's page (or the code expires).
func WaitDeviceCode(code *DeviceCode) (*Account, error) {
	interval := time.Duration(max(code.Interval, 2)) * time.Second
	deadline := time.Now().Add(time.Duration(code.ExpiresIn) * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(interval)
		var token msToken
		err := postForm("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", url.Values{
			"grant_type": {"urn:ietf:params:oauth:grant-type:device_code"}, "client_id": {ClientID}, "device_code": {code.DeviceCode},
		}, &token)
		var he *httpError
		if errors.As(err, &he) && (strings.Contains(he.body, "authorization_pending") || strings.Contains(he.body, "slow_down")) {
			continue
		}
		if err != nil {
			return nil, err
		}
		return minecraftFromMicrosoft(token)
	}
	return nil, errors.New("the code expired: sign in again")
}

// Refresh signs in again with the kept refresh token.
func Refresh(account *Account) (*Account, error) {
	var token msToken
	if err := postForm("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", url.Values{
		"grant_type": {"refresh_token"}, "client_id": {ClientID}, "refresh_token": {account.RefreshToken}, "scope": {scope},
	}, &token); err != nil {
		return nil, err
	}
	return minecraftFromMicrosoft(token)
}

type xboxToken struct {
	Token         string `json:"Token"`
	DisplayClaims struct {
		Xui []struct {
			Uhs string `json:"uhs"`
			Xid string `json:"xid"`
		} `json:"xui"`
	} `json:"DisplayClaims"`
}

func minecraftFromMicrosoft(ms msToken) (*Account, error) {
	var xbl xboxToken
	if err := postJSON("https://user.auth.xboxlive.com/user/authenticate", map[string]any{
		"Properties":   map[string]any{"AuthMethod": "RPS", "SiteName": "user.auth.xboxlive.com", "RpsTicket": "d=" + ms.AccessToken},
		"RelyingParty": "http://auth.xboxlive.com", "TokenType": "JWT",
	}, &xbl, ""); err != nil {
		return nil, fmt.Errorf("xbox live: %w", err)
	}
	var xsts xboxToken
	if err := postJSON("https://xsts.auth.xboxlive.com/xsts/authorize", map[string]any{
		"Properties":   map[string]any{"SandboxId": "RETAIL", "UserTokens": []string{xbl.Token}},
		"RelyingParty": "rp://api.minecraftservices.com/", "TokenType": "JWT",
	}, &xsts, ""); err != nil {
		if strings.Contains(err.Error(), "2148916233") {
			return nil, errors.New("this Microsoft account has no Xbox profile: sign in once at minecraft.net")
		}
		return nil, fmt.Errorf("xsts: %w", err)
	}
	if len(xsts.DisplayClaims.Xui) == 0 {
		return nil, errors.New("xsts: no user hash")
	}
	uhs := xsts.DisplayClaims.Xui[0].Uhs
	var mc struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
	}
	if err := postJSON("https://api.minecraftservices.com/authentication/login_with_xbox",
		map[string]string{"identityToken": "XBL3.0 x=" + uhs + ";" + xsts.Token}, &mc, ""); err != nil {
		if strings.Contains(err.Error(), "403") {
			return nil, fmt.Errorf("minecraft refused the sign-in (the Azure app may not be approved for Minecraft's API yet): %w", err)
		}
		return nil, fmt.Errorf("minecraft: %w", err)
	}
	req, _ := http.NewRequest(http.MethodGet, "https://api.minecraftservices.com/minecraft/profile", nil)
	req.Header.Set("Authorization", "Bearer "+mc.AccessToken)
	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, errors.New("this account doesn't own Minecraft: Java Edition")
	}
	var profile struct {
		ID   string `json:"id"`
		Name string `json:"name"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&profile); err != nil {
		return nil, err
	}
	xuid := ""
	if len(xbl.DisplayClaims.Xui) > 0 {
		xuid = xbl.DisplayClaims.Xui[0].Xid
	}
	return &Account{Name: profile.Name, UUID: profile.ID, Xuid: xuid, RefreshToken: ms.RefreshToken, AccessToken: mc.AccessToken,
		Expires: time.Now().Add(time.Duration(mc.ExpiresIn) * time.Second)}, nil
}
