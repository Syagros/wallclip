// njs module for the wallclip web page: nginx runs this with js_content.
// /_home scans the gallery directory and renders the landing grid (the
// index page pulls it in via SSI); /search asks the unix socket for
// matches via the internal /_q/ proxy and renders the result page.
//
// The socket answers GET /<query> with newline-separated ABSOLUTE thumbnail
// paths under the gallery root (e.g. /mnt/HDD/Wallpapers-Flat/.thumbnails/x.jpg).
// Previews come from .thumbnails (sharing the original's filename); the
// download link targets the original. The /files/ alias is rooted at the
// gallery root, so returned paths must lose that prefix.
var GALLERY_ROOT = "/mnt/HDD/Wallpapers-Flat";

function escape_html(s) {
    return s.replace(/&/g, "\x26amp;").replace(/</g, "\x26lt;")
            .replace(/>/g, "\x26gt;").replace(/"/g, "\x26quot;");
}

// Escape each path segment so names with spaces, '"' or '&' stay valid in
// HTML attributes and URLs.
function escape_uri_path(path) {
    return path.split("/").map(encodeURIComponent).join("/");
}

// Original path -> its preview URL: thumbnails live in a .thumbnails
// directory next to the original and share its filename.
function thumb_from_original(orig) {
    var name = orig.slice(orig.lastIndexOf("/") + 1);
    return orig.slice(0, orig.lastIndexOf("/") + 1) + ".thumbnails/" + name;
}

// Absolute original path -> URL under the /files/ alias. The alias is rooted
// at GALLERY_ROOT, so the path must lose that prefix.
function files_url(path) {
    return "/files" + escape_uri_path(path.slice(GALLERY_ROOT.length));
}

function cards(origs) {
    return origs.map(function (orig) {
        var thumb = files_url(thumb_from_original(orig));
        var full = files_url(orig);
        var name = escape_html(orig.slice(orig.lastIndexOf("/") + 1));
        return "  <div class=\"card\">" +
               "<img src=\"" + thumb + "\" alt=\"" + name + "\" loading=\"lazy\">" +
               "<a class=\"dl\" href=\"" + full + "\" download=\"" + name + "\">Download</a>" +
               "</div>";
    }).join("\n");
}

// Full page shell; the grid content is shared with the landing include.
function page(grid) {
    return "<!doctype html>\n" +
"<html lang=\"en\">\n" +
"<head>\n" +
"<meta charset=\"utf-8\">\n" +
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n" +
"<title>wallclip</title>\n" +
"<link rel=\"stylesheet\" href=\"/style.css\">\n" +
"</head>\n" +
"<body>\n" +
"<header>\n" +
"  <a class=\"brand\" href=\"/\">wall<b>clip</b></a>\n" +
"  <form action=\"/search\" method=\"get\">\n" +
"    <div class=\"searchbar\">\n" +
"      <input type=\"search\" name=\"q\" placeholder=\"Search images…\" aria-label=\"Search\">\n" +
"      <button type=\"submit\">Search</button>\n" +
"    </div>\n" +
"  </form>\n" +
"  <div class=\"spacer\"></div>\n" +
"</header>\n" +
"\n" +
"<main class=\"grid\">\n" + grid + "\n" +
"</main>\n" +
"</body>\n" +
"</html>";
}

// r.return() with a string body defaults to text/plain, which browsers
// download instead of rendering
function reply_html(r, html) {
    r.headersOut["Content-Type"] = "text/html; charset=utf-8";
    r.return(200, html);
}

function render(r) {
    var q = r.args.q ? r.args.q.trim() : "";
    if (!q) {
        reply_html(r, page("  <div class=\"empty\">Type something to search</div>"));
        return;
    }

    // nginx decodes the subrequest URI, so the socket receives "GET /<q>",
    // exactly what the /_q/ proxy passes it.
    r.subrequest("/_q/" + encodeURIComponent(q), function (reply) {
        var origs = [];
        if (reply.status == 200) {
            reply.responseText.split("\n").forEach(function (line) {
                // the socket returns thumbnail paths; the cards take originals
                if (line) origs.push(line.replace("/.thumbnails/", "/"));
            });
        }
        reply_html(r, page(origs.length
            ? cards(origs)
            : "  <div class=\"empty\">No results</div>"));
    });
}

// Gallery contents for the landing page: a directory scan (depth 1), sorted
// for a stable order. The .thumbnails directory is not a gallery item. An
// unreadable gallery renders an empty page instead of failing the request.
// The whole grid is rendered in one go; the images carry loading="lazy", so
// the browser fetches previews only as they scroll into view — no script
// needed on the page.
function list_gallery() {
    var names = [];
    try {
        var fs = require("fs");
        fs.readdirSync(GALLERY_ROOT).forEach(function (name) {
            // dotfiles and .thumbnails hold previews, not gallery items
            if (name[0] !== ".") names.push(GALLERY_ROOT + "/" + name);
        });
    } catch (e) {
        // gallery not mounted or unreadable
    }
    names.sort();
    return names;
}

function home(r) {
    var paths = list_gallery();
    reply_html(r, paths.length
        ? cards(paths)
        : "<div class=\"empty\">Gallery is empty</div>");
}

// njs only supports default exports; js_content references the object's
// properties (search.render, search.home).
export default { render, home };