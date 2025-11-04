import os
import json
import hashlib
import base64
import time
import requests
from datetime import datetime, timedelta
from functools import wraps
from flask import Flask, request, jsonify, render_template, redirect, url_for, session, flash
from Crypto.Cipher import AES
from Crypto.Util.Padding import pad, unpad
import threading
import psycopg2
from psycopg2.extras import RealDictCursor
import sys

# Setting up the Flask application
app = Flask(__name__)
app.secret_key = 'your-secret-key-change-in-production'  # Change this in production! (this is the secret key for the Flask application)

# PostgreSQL Database configuration and setting for the database connection
POSTGRES_CONFIG = {
    'host': 'localhost',
    'database': 'iotdb',
    'user': 'iotuser',
    'password': 'iotpassword',  # Use the same password as the one in the database or change this to your PostgreSQL password
    'port': 5432
}

# Below is the function to get the PostgreSQL database connection
def get_db_connection():
    """Get PostgreSQL database connection"""
    try:
        conn = psycopg2.connect(**POSTGRES_CONFIG)  # This is the connection to the database as per the given configuration
        return conn
    except psycopg2.OperationalError as e:
        print(f"❌ Database connection error: {e}")
        sys.exit(1)

# Below is the AES encryption configuration (must match ESP32)
ENCRYPTION_KEY = b'MySecretKey12345'  # 16 bytes key for AES-128
BLOCK_SIZE = 16

# Below is the Weather API configuration and saving the cache for the weather data
WEATHER_API_KEY = '6dce1f987e127519e7d215654bc1916d'  # OpenWeatherMap API key
WEATHER_CACHE_DURATION = 600  # 10 minutes cache
weather_cache = {}

# Below is the function to initialize the PostgreSQL database with the required tables, indexes, and default admin user
def init_database():
    """Initialize PostgreSQL database with required tables"""
    conn = get_db_connection()
    iotdb = conn.cursor()
    
    # Create sensor data table to store sensor data and timestamp and encrypted data
    iotdb.execute('''
        CREATE TABLE IF NOT EXISTS sensor_data (
            id SERIAL PRIMARY KEY,
            team_number VARCHAR(50) NOT NULL,
            temperature DECIMAL(5,2) NOT NULL,
            humidity DECIMAL(5,2) NOT NULL,
            timestamp BIGINT NOT NULL,
            encrypted_data TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Below is to create the users table to store user information and authentication
    iotdb.execute('''
        CREATE TABLE IF NOT EXISTS users (
            id SERIAL PRIMARY KEY,
            username VARCHAR(100) UNIQUE NOT NULL,
            password_hash VARCHAR(255) NOT NULL,
            role VARCHAR(50) DEFAULT 'user',
            active_sessions INTEGER DEFAULT 0,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    
    # Below is to create indexes for better performance and for faster retrieval of data
    iotdb.execute('CREATE INDEX IF NOT EXISTS idx_sensor_timestamp ON sensor_data(timestamp)')
    iotdb.execute('CREATE INDEX IF NOT EXISTS idx_sensor_team ON sensor_data(team_number)')
    iotdb.execute('CREATE INDEX IF NOT EXISTS idx_users_username ON users(username)')
    
    # To insert the default admin user if not exists
    admin_password = hash_password('admin123')
    iotdb.execute('''
        INSERT INTO users (username, password_hash, role) 
        VALUES (%s, %s, %s)
        ON CONFLICT (username) DO NOTHING
    ''', ('admin', admin_password, 'admin'))
    
    conn.commit()
    conn.close()
    print("Database initialized successfully!")

# This function is to hash the password using SHA-256
def hash_password(password):
    """Hash password using SHA-256 (in production, use bcrypt or Argon2)"""
    return hashlib.sha256(password.encode()).hexdigest()

# This function is to verify the password with the hashed password
def verify_password(password, password_hash):
    """Verify password against hash"""
    return hash_password(password) == password_hash

# Below is the function to decrypt the data from the ESP32
def decrypt_data(encrypted_data):
    """Decrypt AES-encrypted data from ESP32"""
    try:
        # URL decode the encrypted data (in case it was URL encoded during HTTP POST)
        from urllib.parse import unquote
        encrypted_data = unquote(encrypted_data)
        
        # Clean the base64 string (remove any whitespace or invalid characters)
        encrypted_data = encrypted_data.strip().replace('\n', '').replace('\r', '').replace(' ', '+')
        
        # This is to check if base64 string length is valid
        if len(encrypted_data) % 4 != 0:
            print(f"Warning: Base64 string length {len(encrypted_data)} not multiple of 4")
            # Pad with '=' characters
            encrypted_data += '=' * (4 - len(encrypted_data) % 4)
        
        # This is to decode the base64 string
        combined_data = base64.b64decode(encrypted_data)
        
        print(f"Debug: Combined data length: {len(combined_data)} bytes")
        
        # This is to check if data is long enough for IV + ciphertext
        if len(combined_data) < 16:
            print("Error: Encrypted data too short")
            return None
        
        # Below logic is to extract the IV and ciphertext (IV is first 16 bytes)
        iv = combined_data[:16]
        ciphertext = combined_data[16:]
        
        print(f"Debug: IV length: {len(iv)}, Ciphertext length: {len(ciphertext)}")
        
        # Check if ciphertext length is multiple of 16
        if len(ciphertext) % 16 != 0:
            print(f"Warning: Ciphertext length {len(ciphertext)} not multiple of 16")
            # Don't pad - this might be the issue
            print("Debug: Skipping decryption due to invalid ciphertext length")
            return None
        
        # Below is to decrypt the data using AES-128-CBC
        cipher = AES.new(ENCRYPTION_KEY, AES.MODE_CBC, iv)
        decrypted_padded = cipher.decrypt(ciphertext)
        
        print(f"Debug: Decrypted padded length: {len(decrypted_padded)}")
        
        # Below is the error handling to remove padding
        try:
            decrypted = unpad(decrypted_padded, BLOCK_SIZE)
            print("Debug: Successfully removed padding")
        except ValueError as e:
            print(f"Padding error: {e}")
            # This will try manually to remove padding
            decrypted = decrypted_padded.rstrip(b'\x00')
            # Remove PKCS7 padding manually
            if len(decrypted) > 0:
                padding_length = decrypted[-1]
                if padding_length <= 16 and padding_length > 0:
                    decrypted = decrypted[:-padding_length]
            print("Debug: Manually removed padding")
        
        # This is to try to decode as UTF-8 with error handling
        try:
            result = decrypted.decode('utf-8')  # This is to decode the data as UTF-8
            print(f"Debug: Final decrypted string: {result}")
            return result
        except UnicodeDecodeError as e:
            print(f"UTF-8 decode error: {e}")
            # This is to try to decode with error replacement
            result = decrypted.decode('utf-8', errors='replace')  # This is to decode the data as UTF-8 with error replacement
            print(f"Debug: Decoded with replacement: {result}")
            return result
        
    except Exception as e:
        print(f"Decryption error: {e}")
        return None

# Below is the decorator to require login for protected routes, 
# this is to prevent unauthorized access to the dashboard
def login_required(f):
    """Decorator to require login for protected routes"""
    @wraps(f)
    def decorated_function(*args, **kwargs):
        if 'user_id' not in session:
            return redirect(url_for('login'))
        return f(*args, **kwargs)
    return decorated_function

# Below is the route to the dashboard 
# This is to redirect to the dashboard if the user is logged in, otherwise to the login page
@app.route('/')
def index():
    """Redirect to dashboard if logged in, otherwise to login"""
    if 'user_id' in session:
        return redirect(url_for('dashboard'))
    return redirect(url_for('login'))

# Below is the route to the login page
# This is to display the login page and handle the login form submission
@app.route('/login', methods=['GET', 'POST'])
def login():
    """User login page"""
    if request.method == 'POST':
        username = request.form.get('username')
        password = request.form.get('password')
        
        # Below is to commit the database connection and also to execute the query to get the user information from the database
        conn = get_db_connection()
        iotdb = conn.cursor(cursor_factory=RealDictCursor)
        iotdb.execute('SELECT id, password_hash, role, active_sessions FROM users WHERE username = %s', (username,))
        user = iotdb.fetchone()
        conn.close()
        
        # Below is to verify the password with the hashed password
        if user and verify_password(password, user['password_hash']):
            # To update the active sessions for the user in the database for every login
            conn = get_db_connection()
            iotdb = conn.cursor()
            iotdb.execute('UPDATE users SET active_sessions = active_sessions + 1 WHERE id = %s', (user['id'],))
            conn.commit()
            conn.close()
            
            session['user_id'] = user['id']
            session['username'] = username
            session['role'] = user['role']
            flash('Login successful!', 'success')
            return redirect(url_for('dashboard'))
        else:
            flash('Invalid username or password!', 'error')
    
    return render_template('login.html')

# Below is the route to the logout page
# This is to handle the logout form submission and clear the session
@app.route('/logout')
def logout():
    """User logout"""
    # Below is to decrement the active sessions for the user in the database for every logout
    if 'user_id' in session:
        try:
            conn = get_db_connection()
            iotdb = conn.cursor()
            iotdb.execute('UPDATE users SET active_sessions = GREATEST(active_sessions - 1, 0) WHERE id = %s', (session['user_id'],))
            conn.commit()
            conn.close()
        except Exception as e:
            print(f"Error updating active sessions on logout: {e}")
    
    session.clear()
    flash('Logged out successfully!', 'info')
    return redirect(url_for('login')) # This is to redirect to the login page after logout

# Below is the route to the forgot password page
# This is to display the forgot password page and handle the forgot password form submission
# This will also hit the direct change in the database to update the password
@app.route('/forgot-password', methods=['GET', 'POST'])
def forgot_password():
    """Forgot password page to reset user password"""
    if request.method == 'POST':
        username = request.form.get('username')
        new_password = request.form.get('new_password')
        confirm_password = request.form.get('confirm_password')
        
        if not username or not new_password or not confirm_password:
            flash('All fields are required!', 'error')
            return render_template('forgot_password.html')
        
        if new_password != confirm_password:
            flash('Passwords do not match!', 'error')
            return render_template('forgot_password.html')
        
        # Below is to update the password in the database
        conn = get_db_connection()
        iotdb = conn.cursor()
        password_hash = hash_password(new_password)
        iotdb.execute('UPDATE users SET password_hash = %s WHERE username = %s', (password_hash, username))
        conn.commit()
        affected_rows = iotdb.rowcount
        conn.close()
        
        # Logic to check if the password was updated successfully
        if affected_rows > 0:
            flash('Password updated successfully! Please login with your new password.', 'success')
            return redirect(url_for('login'))
        else:
            flash('Username not found!', 'error')
            return render_template('forgot_password.html')
    
    return render_template('forgot_password.html')

# Below is the route to the dashboard page
# This is to display the dashboard page and handle the dashboard requests
@app.route('/dashboard')
@login_required
def dashboard():
    """Main dashboard page"""
    return render_template('dashboard.html', 
                         username=session.get('username'),
                         role=session.get('role', 'user'))

# Below is the route to the admin page
# This is to display the admin page and handle the admin requests and also to check if the user is an admin
@app.route('/admin')
@login_required
def admin():
    """Admin page for user management and analytics"""
    if session.get('role') != 'admin':
        flash('Access denied: Admin privileges required', 'error')
        return redirect(url_for('dashboard'))
    
    return render_template('admin.html', 
                         username=session.get('username'),
                         role=session.get('role'))

# Below is the route to receive the sensor data from the ESP32
# Also to receive the sensor data from the ESP32 and store it in the database and also to decrypt the data
@app.route('/api/sensor-data', methods=['POST'])
def receive_sensor_data():
    """Receive encrypted sensor data from ESP32"""
    try:
        data = request.form
        
        # Extract raw values 
        team_number = data.get('team_number')
        temperature = float(data.get('temperature', 0))
        humidity = float(data.get('humidity', 0))
        timestamp = int(data.get('timestamp', 0))
        is_encrypted = data.get('is_encrypted', 'false').lower() == 'true'
        encrypted_data = data.get('encrypted_data', '')
        
        # Validate humidity range (0-100%) to prevent invalid values
        if humidity < 0.0:
            humidity = 0.0
        if humidity > 100.0:
            humidity = 100.0
        
        # To decrypt the data if encrypted
        if is_encrypted and encrypted_data:
            decrypted_json = decrypt_data(encrypted_data)
            if decrypted_json: # This is to check if the data was decrypted successfully
                try:
                    decrypted_data = json.loads(decrypted_json)
                    temperature = float(decrypted_data.get('temperature', temperature))
                    humidity = float(decrypted_data.get('humidity', humidity))
                    timestamp = int(decrypted_data.get('timestamp', timestamp))
                    team_number = decrypted_data.get('team_number', team_number)
                    # Validate humidity range after decryption
                    if humidity < 0.0:
                        humidity = 0.0
                    if humidity > 100.0:
                        humidity = 100.0
                    print(f"Successfully decrypted data: Temp={temperature}, Hum={humidity}")
                except json.JSONDecodeError as e:
                    print(f"JSON decode error: {e}")
                    print(f"Decrypted string: {decrypted_json}")
            else:
                print("Failed to decrypt data, using unencrypted values")
        
        
        # To store the sensor data in the database
        # Convert ESP32's Unix timestamp to PostgreSQL TIMESTAMP for created_at
        created_at_timestamp = datetime.fromtimestamp(timestamp)
        conn = get_db_connection()
        iotdb = conn.cursor()
        iotdb.execute('''
            INSERT INTO sensor_data (team_number, temperature, humidity, timestamp, encrypted_data, created_at)
            VALUES (%s, %s, %s, %s, %s, %s)
        ''', (team_number, temperature, humidity, timestamp, encrypted_data, created_at_timestamp))
        conn.commit()
        conn.close()
        
        #print(f"Data received and stored: Temp={temperature}, Hum={humidity}, Team={team_number}, Timestamp={timestamp}")
        sensor_payload = {
            "team_number": team_number, "temperature": round(float(temperature), 2), "humidity": round(float(humidity), 2), "timestamp": timestamp, "timestamp_iso": datetime.fromtimestamp(timestamp).strftime('%Y-%m-%d %H:%M:%S'), "encrypted": is_encrypted,
            "source": "decrypted" if is_encrypted and encrypted_data else "plaintext"
        }

        print("SENSOR DATA RECEIVED (ESP32)") # This is to print the sensor data received from the ESP32 in terminal
        print(json.dumps(sensor_payload, indent=2))

        return jsonify({
            'status': 'success',
            'message': 'Data received and stored',
            'timestamp': timestamp
        })
        
    except Exception as e:
        # This is to return the error message if the sensor data was not received successfully
        return jsonify({
            'status': 'error',
            'message': str(e)
        }), 500

# Below is the route to get the current sensor data
# To get the current sensor data from the database and return it as a JSON object
@app.route('/api/current-data')
@login_required
def get_current_data():
    """Get the most recent sensor data"""
    conn = get_db_connection()
    iotdb = conn.cursor()
    iotdb.execute('''
        SELECT temperature, humidity, timestamp, team_number
        FROM sensor_data
        ORDER BY timestamp DESC
        LIMIT 1
    ''')
    data = iotdb.fetchone()
    conn.close()
    
    if data:
        return jsonify({
            'temperature': float(data[0]),
            'humidity': float(data[1]),
            'timestamp': data[2],
            'team_number': data[3]
        })
    else:
        return jsonify({
            'temperature': 0,
            'humidity': 0,
            'timestamp': 0,
            'team_number': ''
        })

# Below is the route to get the historical sensor data
# To get the historical sensor data from the database and return it as a JSON object
# This will return team number, temperature, humidity, and timestamp
@app.route('/api/historical-data')
@login_required
def get_historical_data():
    """Get historical sensor data with optional filtering"""
    limit = request.args.get('limit', 100, type=int)
    team_filter = request.args.get('team', '')
    start_date = request.args.get('start_date', '')
    end_date = request.args.get('end_date', '')
    
    conn = get_db_connection()
    iotdb = conn.cursor()
    
    query = '''
        SELECT temperature, humidity, timestamp, team_number
        FROM sensor_data
        WHERE 1=1
    '''
    params = []
    
    if team_filter:
        query += ' AND team_number = %s'
        params.append(team_filter)
    
    if start_date:
        query += ' AND timestamp >= %s'
        params.append(int(datetime.strptime(start_date, '%Y-%m-%d').timestamp()))
    
    if end_date:
        query += ' AND timestamp <= %s'
        params.append(int(datetime.strptime(end_date, '%Y-%m-%d').timestamp()))
    
    query += ' ORDER BY timestamp DESC LIMIT %s'
    params.append(limit)
    
    iotdb.execute(query, params)
    data = iotdb.fetchall()
    conn.close()
    
    return jsonify([{
        'temperature': float(row[0]),
        'humidity': float(row[1]),
        'timestamp': row[2],
        'team_number': row[3]
    } for row in data])

# Below is the route to search the sensor data by temperature or humidity threshold
# example: http://127.0.0.1:8888/api/search-data?threshold=25&comparison=above&team=9&metric=temperature
@app.route('/api/search-data')
@login_required
def search_data():
    """Search sensor data by temperature or humidity threshold"""
    threshold = request.args.get('threshold', type=float)
    comparison = request.args.get('comparison', 'above')  # 'above' or 'below'
    team_filter = request.args.get('team', '')
    metric = request.args.get('metric', 'temperature')  # 'temperature' or 'humidity'
    
    if threshold is None:
        return jsonify({'error': 'Threshold parameter required'}), 400
    
    conn = get_db_connection()
    iotdb = conn.cursor()
    
    # Build the WHERE clause based on metric (temperature or humidity)
    metric_column = metric  # 'temperature' or 'humidity'
    
    if comparison == 'above':
        if team_filter:
            iotdb.execute(f'''
                SELECT temperature, humidity, timestamp, team_number
                FROM sensor_data
                WHERE {metric_column} > %s AND team_number = %s
                ORDER BY timestamp DESC
            ''', (threshold, team_filter))
        else:
            iotdb.execute(f'''
                SELECT temperature, humidity, timestamp, team_number
                FROM sensor_data
                WHERE {metric_column} > %s
                ORDER BY timestamp DESC
            ''', (threshold,))
    else:  # below
        if team_filter:
            iotdb.execute(f'''
                SELECT temperature, humidity, timestamp, team_number
                FROM sensor_data
                WHERE {metric_column} < %s AND team_number = %s
                ORDER BY timestamp DESC
            ''', (threshold, team_filter))
        else:
            iotdb.execute(f'''
                SELECT temperature, humidity, timestamp, team_number
                FROM sensor_data
                WHERE {metric_column} < %s
                ORDER BY timestamp DESC
            ''', (threshold,))
    
    data = iotdb.fetchall()
    conn.close()
    
    return jsonify([{
        'temperature': float(row[0]),
        'humidity': float(row[1]),
        'timestamp': row[2],
        'team_number': row[3]
    } for row in data])

# Below is the function to get the weather data from the OpenWeatherMap API
def get_weather_data(lat=None, lon=None, city=None):
    """Get weather data from OpenWeatherMap API"""
    if not WEATHER_API_KEY:
        return None
    
    # This is to check the cache first, if the cache is not found, then it will fetch the data from the OpenWeatherMap API
    cache_key = f"{lat}_{lon}_{city}" if city else f"{lat}_{lon}"
    # This is to check if the cache is found, if it is found, then it will return the cached data
    if cache_key in weather_cache:
        cached_data, cached_time = weather_cache[cache_key]
        if time.time() - cached_time < WEATHER_CACHE_DURATION: # This is to check if the cache is expired
            return cached_data
    
    try: # This is to try to fetch the data from the OpenWeatherMap API
        # OpenWeatherMap API endpoint
        if city:
            url = f"http://api.openweathermap.org/data/2.5/weather?q={city}&appid={WEATHER_API_KEY}&units=metric"
        else:
            url = f"http://api.openweathermap.org/data/2.5/weather?lat={lat}&lon={lon}&appid={WEATHER_API_KEY}&units=metric"
        
        response = requests.get(url, timeout=10)
        if response.status_code == 200: # This is to check if the data was fetched successfully
            data = response.json()
            weather_data = {
                'temperature': data['main']['temp'],
                'humidity': data['main']['humidity'],
                'condition': data['weather'][0]['description'],
                'icon': f"http://openweathermap.org/img/w/{data['weather'][0]['icon']}.png",
                'location': data['name'],
                'updated': datetime.now().isoformat()
            }
            
            # This is to cache the data for 10 minutes
            weather_cache[cache_key] = (weather_data, time.time())
            return weather_data
    except Exception as e:
        print(f"Weather API error: {e}")
    
    return None

# Below is the route to get the weather data from the OpenWeatherMap API
@app.route('/api/weather')
@login_required
def get_weather():
    """Get weather data endpoint"""
    # This is to get the latitude, longitude, and city from the request parameters
    lat = request.args.get('lat', type=float)
    lon = request.args.get('lon', type=float)
    city = request.args.get('city', '')
    
    weather_data = get_weather_data(lat, lon, city)
    
    # This is to check if the data was fetched successfully, if it is, then it will return the data as a JSON object
    if weather_data:
        return jsonify(weather_data)
    else:
        return jsonify({'error': 'Weather data not available'}), 503

# Below is the route to create a new user
# This is to create a new user in the database and also to check if the user is an admin or not
# User creation can be done by the admin only
@app.route('/api/users', methods=['POST'])
@login_required
def create_user():
    """Create a new user (admin only)"""
    if session.get('role') != 'admin':
        return jsonify({'error': 'Unauthorized'}), 403
    
    # This is to accept the JSON data from the request
    data = request.get_json()
    username = data.get('username') if data else None
    password = data.get('password') if data else None
    role = data.get('role', 'user') if data else 'user'
    
    if not username or not password:
        return jsonify({'error': 'Username and password required'}), 400
    
    # This is to get the database connection and also to create a new user in the database
    conn = get_db_connection()
    iotdb = conn.cursor()
    
    try:
        password_hash = hash_password(password)
        iotdb.execute('''
            INSERT INTO users (username, password_hash, role)
            VALUES (%s, %s, %s)
            RETURNING id
        ''', (username, password_hash, role))
        user_id = iotdb.fetchone()[0]
        conn.commit()
        
        return jsonify({
            'status': 'success',
            'message': 'User created successfully',
            'user_id': user_id
        }), 201
    except psycopg2.IntegrityError:
        conn.rollback()
        return jsonify({'error': 'Username already exists'}), 400
    except Exception as e:
        conn.rollback()
        return jsonify({'error': str(e)}), 500
    finally:
        conn.close()

# Below is the route to get all the users
# This is to get all the users from the database and return it as a JSON object
# This will return the user id, username, role, active sessions, and created at
@app.route('/api/users')
@login_required
def get_users():
    """Get all users (admin only)"""
    if session.get('role') != 'admin':
        return jsonify({'error': 'Unauthorized'}), 403
    
    # This is to get the database connection and also to get all the users from the database
    conn = get_db_connection()
    iotdb = conn.cursor(cursor_factory=RealDictCursor)
    iotdb.execute('SELECT id, username, role, active_sessions, created_at FROM users')
    users = iotdb.fetchall()
    conn.close()
    
    return jsonify([dict(user) for user in users])

# Below is the route to reset all the active sessions to 0
# This is to reset all the active sessions to 0 for all the users in the database
# This will be done by the admin only
@app.route('/api/reset-sessions', methods=['POST'])
@login_required
def reset_sessions():
    """Reset all active sessions to 0 (admin only)"""
    if session.get('role') != 'admin':
        return jsonify({'error': 'Unauthorized'}), 403
    
    try:
        conn = get_db_connection()
        iotdb = conn.cursor()
        iotdb.execute('UPDATE users SET active_sessions = 0')
        conn.commit()
        conn.close()
        
        return jsonify({'success': True, 'message': 'All sessions reset successfully'})
    except Exception as e:
        return jsonify({'error': str(e)}), 500

# Below is the route to get the dashboard statistics
# This is to get the dashboard statistics from the database and return it as a JSON object
# This will return the total records, active connections, throughput, and query efficiency
@app.route('/api/dashboard-stats')
@login_required
def get_dashboard_stats():
    """Get dashboard statistics"""
    conn = get_db_connection()
    iotdb = conn.cursor()
    
    try:
        # This is to get the total records from the sensor data table
        iotdb.execute('SELECT COUNT(*) FROM sensor_data')
        total_records = iotdb.fetchone()[0]
        
        # This is to get the active connections (sum of all active sessions) from the users table
        iotdb.execute('SELECT COALESCE(SUM(active_sessions), 0) FROM users')
        active_connections = iotdb.fetchone()[0]
        
        # To calculate the real data throughput (records per minute in last hour)
        # Use Unix timestamp column to compare with current time minus 60 seconds
        current_timestamp = int(time.time())
        one_minute_ago = current_timestamp - 60
        iotdb.execute('SELECT COUNT(*) FROM sensor_data WHERE timestamp > %s', (one_minute_ago,))
        last_minute_records = iotdb.fetchone()[0]
        throughput = last_minute_records  # Records per minute
        
        # This is to calculate the real query efficiency (percentage of successful queries) based on table size and index usage
        start_time = time.time()
        iotdb.execute('SELECT COUNT(*) FROM sensor_data WHERE temperature > 0')
        query_time = time.time() - start_time
        
        # This is to calculate the efficiency: faster queries = higher efficiency
        # Assuming < 0.01s is 100%, > 1s is 0% (this is a assumption and can be changed)
        if query_time < 0.01:
            query_efficiency = 100
        elif query_time > 1:
            query_efficiency = 50
        else:
            query_efficiency = max(50, int(100 - (query_time * 50)))
        
        conn.close()
        
        return jsonify({
            'total_records': total_records,
            'active_connections': active_connections,
            'throughput': throughput,  # Records per minute
            'query_efficiency': query_efficiency  # Real calculated value
        })
    except Exception as e:
        print(f"Error getting dashboard stats: {e}")
        conn.close()
        return jsonify({
            'total_records': 0,
            'active_connections': 0,
            'throughput': 0,
            'query_efficiency': 0
        })

if __name__ == '__main__':
    # Below is to initialize the database
    # Initialize database
    init_database()
    
    # To create templates directory if it doesn't exist
    os.makedirs('templates', exist_ok=True)
    os.makedirs('static', exist_ok=True)
    
    print("Starting IoT Monitoring Server...")
    print("Dashboard will be available at: http://localhost:8888")
    print("Default login: admin / admin123")
    
    app.run(host='0.0.0.0', port=8888, debug=False)

