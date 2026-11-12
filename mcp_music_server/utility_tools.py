"""Small offline utilities. Every tool has an explicit input schema."""
from collections import Counter
from datetime import date, datetime
import base64
import hashlib
import ipaddress
import json
import math
import re
import secrets
import statistics
import string
from urllib.parse import quote, unquote, urlsplit

TEXT = {"type": "string", "maxLength": 4096}
NUMBER = {"type": "number", "minimum": -1e12, "maximum": 1e12}
NUMBERS = {"type": "array", "items": NUMBER, "minItems": 1, "maxItems": 128}
INTEGER = {"type": "integer", "minimum": 1, "maximum": 1000}
DATE = {"type": "string", "pattern": "^\\d{4}-\\d{2}-\\d{2}$", "maxLength": 10}
TOOLS = {}


def group(category, fields, rows):
    for name, description, fn in rows:
        TOOLS[name] = ({
            "name": name, "description": description, "category": category,
            "inputSchema": {"type": "object", "properties": fields,
                            "required": list(fields), "additionalProperties": False},
        }, fn)


group("Toán", {"a": NUMBER, "b": NUMBER}, [
    ("math_add", "Cộng a và b.", lambda a, b: a + b),
    ("math_subtract", "Trừ b khỏi a.", lambda a, b: a - b),
    ("math_multiply", "Nhân a với b.", lambda a, b: a * b),
    ("math_divide", "Chia a cho b.", lambda a, b: a / b),
    ("math_percent", "Tính b phần trăm của a.", lambda a, b: a * b / 100),
])
group("Đơn vị", {"value": NUMBER}, [
    ("unit_c_to_f", "Đổi độ Celsius sang Fahrenheit.", lambda value: value * 9 / 5 + 32),
    ("unit_f_to_c", "Đổi độ Fahrenheit sang Celsius.", lambda value: (value - 32) * 5 / 9),
    ("unit_km_to_mi", "Đổi km sang dặm.", lambda value: value / 1.609344),
    ("unit_kg_to_lb", "Đổi kg sang pound.", lambda value: value / .45359237),
    ("unit_l_to_gal", "Đổi lít sang gallon Mỹ.", lambda value: value / 3.785411784),
])
group("Văn bản", {"text": TEXT}, [
    ("text_words", "Đếm từ tách bằng khoảng trắng.", lambda text: len(text.split())),
    ("text_chars", "Đếm ký tự Unicode.", lambda text: len(text)),
    ("text_trim", "Gộp khoảng trắng thừa.", lambda text: " ".join(text.split())),
    ("text_upper", "Đổi văn bản sang chữ hoa.", lambda text: text.upper()),
    ("text_lower", "Đổi văn bản sang chữ thường.", lambda text: text.lower()),
])
group("Lịch", {"day": DATE}, [
    ("date_weekday", "Tìm thứ trong tuần cho ngày YYYY-MM-DD.", lambda day: date.fromisoformat(day).isoweekday()),
    ("date_iso_week", "Trả năm và tuần theo ISO cho ngày YYYY-MM-DD.", lambda day: date.fromisoformat(day).isocalendar()[:2]),
    ("date_days_from_today", "Tính số ngày từ hôm nay đến ngày nhập.", lambda day: (date.fromisoformat(day) - date.today()).days),
    ("date_month_start", "Tìm ngày đầu tháng.", lambda day: date.fromisoformat(day).replace(day=1).isoformat()),
    ("date_ordinal", "Tính số thứ tự của ngày trong năm.", lambda day: date.fromisoformat(day).timetuple().tm_yday),
])
group("Thống kê", {"values": NUMBERS}, [
    ("stats_mean", "Tính trung bình của danh sách số.", lambda values: statistics.fmean(values)),
    ("stats_median", "Tính trung vị của danh sách số.", lambda values: statistics.median(values)),
    ("stats_range", "Tính giá trị nhỏ nhất, lớn nhất và khoảng.", lambda values: {"min": min(values), "max": max(values), "range": max(values)-min(values)}),
    ("stats_sum", "Tính tổng của danh sách số.", lambda values: math.fsum(values)),
    ("stats_population_sd", "Tính độ lệch chuẩn tổng thể.", lambda values: statistics.pstdev(values)),
])
group("Mã hóa dữ liệu", {"text": TEXT}, [
    ("encoding_base64", "Mã hóa văn bản UTF-8 thành Base64.", lambda text: base64.b64encode(text.encode()).decode()),
    ("encoding_from_base64", "Giải mã văn bản UTF-8 từ Base64.", lambda text: base64.b64decode(text, validate=True).decode()),
    ("encoding_sha256", "Tính mã SHA-256 của văn bản UTF-8.", lambda text: hashlib.sha256(text.encode()).hexdigest()),
    ("encoding_url_quote", "Mã hóa một thành phần URL.", lambda text: quote(text, safe="")),
    ("encoding_url_unquote", "Giải mã ký tự phần trăm trong URL.", lambda text: unquote(text)),
])
group("JSON", {"text": TEXT}, [
    ("json_check", "Kiểm tra và trả loại dữ liệu JSON.", lambda text: {"valid": True, "type": type(json.loads(text)).__name__}),
    ("json_pretty", "Định dạng JSON để dễ đọc.", lambda text: json.dumps(json.loads(text), ensure_ascii=False, indent=2)),
    ("json_compact", "Nén khoảng trắng của JSON.", lambda text: json.dumps(json.loads(text), ensure_ascii=False, separators=(",", ":"))),
    ("json_keys", "Liệt kê các khóa của đối tượng JSON.", lambda text: list(dict(json.loads(text)).keys())),
    ("json_count", "Đếm phần tử hoặc khóa ở cấp ngoài cùng của JSON.", lambda text: len(json.loads(text))),
])
group("Ngẫu nhiên", {"count": INTEGER}, [
    ("random_die", "Gieo count lần xúc xắc sáu mặt.", lambda count: [1 + secrets.randbelow(6) for _ in range(count)]),
    ("random_coin", "Tung count lần đồng xu.", lambda count: [secrets.choice(("ngửa", "sấp")) for _ in range(count)]),
    ("random_digits", "Tạo chuỗi count chữ số ngẫu nhiên.", lambda count: "".join(secrets.choice(string.digits) for _ in range(count))),
    ("random_letters", "Tạo chuỗi count chữ cái Latin ngẫu nhiên.", lambda count: "".join(secrets.choice(string.ascii_letters) for _ in range(count))),
    ("random_hex", "Tạo chuỗi hex từ count byte ngẫu nhiên.", lambda count: secrets.token_hex(count)),
])
group("Địa chỉ mạng", {"text": TEXT}, [
    ("net_ip_family", "Kiểm tra địa chỉ và xác định IPv4 hoặc IPv6.", lambda text: ipaddress.ip_address(text).version),
    ("net_ip_format", "Viết gọn địa chỉ IP.", lambda text: ipaddress.ip_address(text).compressed),
    ("net_cidr_network", "Tính địa chỉ mạng từ CIDR.", lambda text: str(ipaddress.ip_network(text, strict=False))),
    ("net_cidr_size", "Đếm địa chỉ trong dải CIDR.", lambda text: ipaddress.ip_network(text, strict=False).num_addresses),
    ("net_url_host", "Tách tên host từ URL, không truy cập mạng.", lambda text: urlsplit(text).hostname),
])
group("Danh sách", {"text": TEXT}, [
    ("list_unique_lines", "Bỏ dòng trùng, giữ thứ tự xuất hiện.", lambda text: list(dict.fromkeys(text.splitlines()))),
    ("list_sort_lines", "Sắp xếp các dòng theo chữ cái.", lambda text: sorted(text.splitlines(), key=str.casefold)),
    ("list_reverse_lines", "Đảo thứ tự các dòng.", lambda text: text.splitlines()[::-1]),
    ("list_line_counts", "Đếm số lần xuất hiện của từng dòng.", lambda text: dict(Counter(text.splitlines()))),
    ("list_number_lines", "Đánh số cho từng dòng.", lambda text: [f"{i}. {line}" for i, line in enumerate(text.splitlines(), 1)]),
])


def valid(schema, value):
    kind = schema["type"]
    if kind == "string":
        return (isinstance(value, str) and len(value) <= schema["maxLength"]
                and ("pattern" not in schema or re.fullmatch(schema["pattern"], value) is not None))
    if kind == "array":
        return (isinstance(value, list) and schema["minItems"] <= len(value) <= schema["maxItems"]
                and all(valid(schema["items"], item) for item in value))
    types = (int,) if kind == "integer" else (int, float)
    return type(value) in types and schema["minimum"] <= value <= schema["maximum"]


def run(name, arguments):
    tool, fn = TOOLS[name]
    fields = tool["inputSchema"]["properties"]
    if set(arguments) != set(fields) or any(not valid(spec, arguments[key]) for key, spec in fields.items()):
        raise ValueError("Tham số thiếu, sai kiểu hoặc vượt giới hạn.")
    return fn(**arguments)
