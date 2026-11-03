package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/textproto"
	"net/url"
	"sort"
	"strings"
	"sync"
)

// Minimal Google Drive v3 client (stdlib only) for the real provider.
//
// Folder model (unchanged from §3.3a): one dedicated root folder
// "Onorab_projet", one subfolder per project, no other Drive folder
// ever touched. With drive.file scope this is also a permission
// boundary: the app only ever lists/creates inside folders it created
// (or that were shared with the user for an Onorab project).
//
// DriveClient never holds tokens itself: it asks DriveAuth for a live
// access token per request, so refresh happens transparently and the
// client stays usable across token expiry.

const (
	defaultDriveAPIBase    = "https://www.googleapis.com/drive/v3"
	defaultDriveUploadBase = "https://www.googleapis.com/upload/drive/v3"
	folderMimeType         = "application/vnd.google-apps.folder"
)

type DriveClient struct {
	httpClient *http.Client
	token      func() (string, error)
	apiBase    string
	uploadBase string

	mu         sync.Mutex
	rootID     string
	projectIDs map[string]string
}

func newDriveClient(token func() (string, error)) *DriveClient {
	return &DriveClient{
		httpClient: http.DefaultClient,
		token:      token,
		apiBase:    defaultDriveAPIBase,
		uploadBase: defaultDriveUploadBase,
		projectIDs: map[string]string{},
	}
}

type driveFile struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	MimeType    string `json:"mimeType"`
	Size        string `json:"size"`
	CreatedTime string `json:"createdTime"`
}

func (c *DriveClient) do(method, fullURL string, body io.Reader, contentType string) (*http.Response, error) {
	tok, err := c.token()
	if err != nil {
		return nil, err
	}
	req, err := http.NewRequest(method, fullURL, body)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+tok)
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("Drive request failed: %w", err)
	}
	if resp.StatusCode == http.StatusUnauthorized {
		_, _ = io.Copy(io.Discard, resp.Body)
		_ = resp.Body.Close()
		return nil, fmt.Errorf("Drive authorization expired or revoked — reconnect Drive (Settings → Connect Drive)")
	}
	return resp, nil
}

func driveQuery(s string) string {
	return strings.ReplaceAll(s, "'", "\\'")
}

// listFilesRaw runs one files.list query and returns the page.
func (c *DriveClient) listFilesRaw(q string) ([]driveFile, error) {
	v := url.Values{}
	v.Set("q", q)
	v.Set("fields", "files(id,name,mimeType,size,createdTime)")
	v.Set("pageSize", "100")
	v.Set("orderBy", "createdTime")
	resp, err := c.do(http.MethodGet, c.apiBase+"/files?"+v.Encode(), nil, "")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("Drive list failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var page struct {
		Files []driveFile `json:"files"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&page); err != nil {
		return nil, fmt.Errorf("decode Drive list: %w", err)
	}
	return page.Files, nil
}

// getOrCreateFolder finds a folder by name under parent ("" = Drive
// root) or creates it. Results are cached per Hub run: folder ids do
// not change, and re-listing on every upload would be wasteful.
func (c *DriveClient) getOrCreateFolder(name, parentID string) (string, error) {
	c.mu.Lock()
	if parentID == "" && name == "Onorab_projet" && c.rootID != "" {
		id := c.rootID
		c.mu.Unlock()
		return id, nil
	}
	if parentID != "" {
		if id, ok := c.projectIDs[parentID+"/"+name]; ok {
			c.mu.Unlock()
			return id, nil
		}
	}
	c.mu.Unlock()

	q := "name = '" + driveQuery(name) + "' and mimeType = '" + folderMimeType + "' and trashed = false"
	if parentID != "" {
		q += " and '" + parentID + "' in parents"
	}
	found, err := c.listFilesRaw(q)
	if err != nil {
		return "", err
	}
	if len(found) > 0 {
		id := found[0].ID
		c.mu.Lock()
		if parentID == "" && name == "Onorab_projet" {
			c.rootID = id
		} else if parentID != "" {
			c.projectIDs[parentID+"/"+name] = id
		}
		c.mu.Unlock()
		return id, nil
	}
	meta := map[string]any{"name": name, "mimeType": folderMimeType}
	if parentID != "" {
		meta["parents"] = []string{parentID}
	}
	payload, _ := json.Marshal(meta)
	resp, err := c.do(http.MethodPost, c.apiBase+"/files", bytes.NewReader(payload), "application/json")
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return "", fmt.Errorf("Drive create folder %q failed (%d): %s", name, resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var created driveFile
	if err := json.NewDecoder(resp.Body).Decode(&created); err != nil {
		return "", fmt.Errorf("decode Drive folder create: %w", err)
	}
	c.mu.Lock()
	if parentID == "" && name == "Onorab_projet" {
		c.rootID = created.ID
	} else if parentID != "" {
		c.projectIDs[parentID+"/"+name] = created.ID
	}
	c.mu.Unlock()
	return created.ID, nil
}

func (c *DriveClient) rootFolderID() (string, error) {
	return c.getOrCreateFolder("Onorab_projet", "")
}

func (c *DriveClient) projectFolderID(project string) (string, error) {
	root, err := c.rootFolderID()
	if err != nil {
		return "", err
	}
	if project == "" {
		return root, nil
	}
	return c.getOrCreateFolder(project, root)
}

// ListAll returns every Onorab file in creation order (oldest first),
// matching the order rule: root files first carry no project, then
// each project's files. The final sort is by createdTime, so an
// invited user's upload lands at the end, in order.
func (c *DriveClient) ListAll() ([]OnorabFile, error) {
	root, err := c.rootFolderID()
	if err != nil {
		return nil, err
	}
	var out []OnorabFile
	rootFiles, err := c.listFilesRaw("'" + root + "' in parents and trashed = false and mimeType != '" + folderMimeType + "'")
	if err != nil {
		return nil, err
	}
	for _, f := range rootFiles {
		out = append(out, driveToOnorab(f, nil))
	}
	folders, err := c.listFilesRaw("'" + root + "' in parents and trashed = false and mimeType = '" + folderMimeType + "'")
	if err != nil {
		return nil, err
	}
	for _, folder := range folders {
		project := folder.Name
		files, err := c.listFilesRaw("'" + folder.ID + "' in parents and trashed = false and mimeType != '" + folderMimeType + "'")
		if err != nil {
			return nil, err
		}
		for _, f := range files {
			p := project
			out = append(out, driveToOnorab(f, &p))
		}
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].ModifiedTime < out[j].ModifiedTime })
	return out, nil
}

func driveToOnorab(f driveFile, project *string) OnorabFile {
	var size int64
	_, _ = fmt.Sscanf(f.Size, "%d", &size)
	return OnorabFile{
		ID: f.ID, Name: f.Name, MimeType: f.MimeType, SizeBytes: size,
		ModifiedTime: f.CreatedTime, ProjectID: project, Source: "drive",
	}
}

// Upload stores bytes in the project's folder (creating it on first
// use) and returns the created Drive file as an OnorabFile.
func (c *DriveClient) Upload(project, name string, data []byte, mime string) (OnorabFile, error) {
	folderID, err := c.projectFolderID(project)
	if err != nil {
		return OnorabFile{}, err
	}
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	metaPart, err := mw.CreatePart(textproto.MIMEHeader{"Content-Type": []string{"application/json; charset=UTF-8"}})
	if err != nil {
		return OnorabFile{}, err
	}
	meta, _ := json.Marshal(map[string]any{"name": name, "parents": []string{folderID}})
	if _, err := metaPart.Write(meta); err != nil {
		return OnorabFile{}, err
	}
	if mime == "" {
		mime = "application/octet-stream"
	}
	mediaPart, err := mw.CreatePart(textproto.MIMEHeader{"Content-Type": []string{mime}})
	if err != nil {
		return OnorabFile{}, err
	}
	if _, err := mediaPart.Write(data); err != nil {
		return OnorabFile{}, err
	}
	if err := mw.Close(); err != nil {
		return OnorabFile{}, err
	}
	resp, err := c.do(http.MethodPost, c.uploadBase+"/files?uploadType=multipart", &buf, mw.FormDataContentType())
	if err != nil {
		return OnorabFile{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return OnorabFile{}, fmt.Errorf("Drive upload failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var created driveFile
	if err := json.NewDecoder(resp.Body).Decode(&created); err != nil {
		return OnorabFile{}, fmt.Errorf("decode Drive upload: %w", err)
	}
	var p *string
	if project != "" {
		p = &project
	}
	return driveToOnorab(created, p), nil
}

// GetFile returns one file's metadata (used before Download so the
// Hub can set a correct Content-Type and confirm the id exists).
func (c *DriveClient) GetFile(id string) (driveFile, error) {
	v := url.Values{}
	v.Set("fields", "id,name,mimeType,size,createdTime")
	resp, err := c.do(http.MethodGet, c.apiBase+"/files/"+url.PathEscape(id)+"?"+v.Encode(), nil, "")
	if err != nil {
		return driveFile{}, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return driveFile{}, fmt.Errorf("not_found")
	}
	if resp.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return driveFile{}, fmt.Errorf("Drive get file failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var f driveFile
	if err := json.NewDecoder(resp.Body).Decode(&f); err != nil {
		return driveFile{}, fmt.Errorf("decode Drive file: %w", err)
	}
	return f, nil
}

// Download returns a file's raw bytes.
func (c *DriveClient) Download(id string) ([]byte, error) {
	resp, err := c.do(http.MethodGet, c.apiBase+"/files/"+url.PathEscape(id)+"?alt=media", nil, "")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, fmt.Errorf("not_found")
	}
	if resp.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return nil, fmt.Errorf("Drive download failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	return io.ReadAll(resp.Body)
}

// Delete trashes (Drive delete) one file by Drive id.
func (c *DriveClient) Delete(id string) error {
	resp, err := c.do(http.MethodDelete, c.apiBase+"/files/"+url.PathEscape(id), nil, "")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return fmt.Errorf("not_found")
	}
	if resp.StatusCode != http.StatusNoContent && resp.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("Drive delete failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	return nil
}

// Share grants one Gmail writer access to a project's folder — the
// real form of an invite. It returns the permission id. Note for the
// invitee: with drive.file scope, their own Hub sees the shared folder
// once Drive has it in their "shared with me" view; the folder is
// listed under Onorab_projet projects only if they open it once, which
// is a drive.file limitation the plugin's invite status text states.
func (c *DriveClient) Share(project, email string) (string, error) {
	folderID, err := c.projectFolderID(project)
	if err != nil {
		return "", err
	}
	payload, _ := json.Marshal(map[string]string{"type": "user", "role": "writer", "emailAddress": email})
	resp, err := c.do(http.MethodPost, c.apiBase+"/files/"+url.PathEscape(folderID)+"/permissions", bytes.NewReader(payload), "application/json")
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return "", fmt.Errorf("Drive share failed (%d): %s", resp.StatusCode, strings.TrimSpace(string(data)))
	}
	var perm struct {
		ID string `json:"id"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&perm); err != nil {
		return "", fmt.Errorf("decode Drive permission: %w", err)
	}
	return perm.ID, nil
}
